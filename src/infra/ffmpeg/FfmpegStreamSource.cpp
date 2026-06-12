#include "FfmpegStreamSource.h"
#include "HwContext.h"

#include <cstdio>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#endif

#include <cerrno>
#include <chrono>
#include <vector>

namespace nv::infra {

using nv::domain::DiagnosisReason;

namespace {
int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

DiagnosisReason mapOpenError(int rc) {
    switch (rc) {
        case AVERROR(ETIMEDOUT):
        case AVERROR(EHOSTUNREACH):
        case AVERROR(ENETUNREACH):
        case AVERROR(ECONNREFUSED):
            return DiagnosisReason::DeviceUnreachable;
        default:
            return DiagnosisReason::SessionRefused;   // RTSP 협상/포맷 실패 일반
    }
}

#if defined(__APPLE__)
// CVPixelBufferRetain/Release를 void* 시그니처로 슬롯에 주입 (수명 격리, LatestSurfaceSlot 참조).
void* cvRetain(void* p) {
    return CVPixelBufferRetain(static_cast<CVPixelBufferRef>(p));
}
void cvRelease(void* p) {
    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(p));
}
#endif
} // namespace

FfmpegStreamSource::FfmpegStreamSource(LatestSurfaceSlot& frameSlot) : m_frameSlot(frameSlot) {
    avformat_network_init();
#if defined(__APPLE__)
    m_frameSlot.setGpuRefcounters(&cvRetain, &cvRelease);
#endif
}

FfmpegStreamSource::~FfmpegStreamSource() { close(); }

int FfmpegStreamSource::interruptCb(void* opaque) {
    auto* self = static_cast<FfmpegStreamSource*>(opaque);
    if (!self->m_stop.load()) return 0;
    // 종료 요청 후에도 close 직전 설정된 짧은 유예 동안은 I/O를 허용한다 —
    // RTSP TEARDOWN 핸드셰이크가 나갈 시간 (유령 세션 방지, docs/issues/2026-06-12 참조)
    return steadyNowMs() < self->m_graceUntilMs.load() ? 0 : 1;
}

void FfmpegStreamSource::open(const std::string& url, nv::app::StreamSourceListener& listener) {
    close();                                   // 이전 사이클 정리 (재진입 안전)
    m_stop = false;
    m_graceUntilMs = 0;
    m_thread = std::thread(&FfmpegStreamSource::run, this, url, &listener);
}

void FfmpegStreamSource::close() {
    m_stop = true;                             // interrupt callback이 블로킹 호출을 깨운다
    if (m_thread.joinable()) m_thread.join();
}

void FfmpegStreamSource::run(std::string url, nv::app::StreamSourceListener* listener) {
    AVFormatContext* fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = &FfmpegStreamSource::interruptCb;
    fmt->interrupt_callback.opaque = this;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "rtsp,rtp,tcp,udp", 0);  // S1: 허용 프로토콜만 열거
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);   // R4: 로컬 레그 TCP 강제
    av_dict_set(&opts, "timeout", "5000000", 0);      // 소켓 타임아웃 5s(us) — FFmpeg 5+
    av_dict_set(&opts, "stimeout", "5000000", 0);     // 구버전 옵션명 (무시돼도 무해)

    int rc = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0) {                              // 실패 시 fmt는 FFmpeg이 해제한다
        if (!m_stop) listener->onSourceError(mapOpenError(rc));
        return;
    }

    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        if (!m_stop) listener->onSourceError(DiagnosisReason::SessionRefused);
        m_graceUntilMs = steadyNowMs() + 1000;   // 읽기 중단 후 TEARDOWN 송신 유예 (2단계)
        avformat_close_input(&fmt);
        return;
    }

    const AVCodec* codec = nullptr;
    const int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (vIdx < 0 || codec == nullptr) {
        if (!m_stop) listener->onSourceError(DiagnosisReason::SessionRefused);
        m_graceUntilMs = steadyNowMs() + 1000;   // 읽기 중단 후 TEARDOWN 송신 유예 (2단계)
        avformat_close_input(&fmt);
        return;
    }

    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[vIdx]->codecpar);

    // M2b: HW 디코딩 시도. 성공 시 get_format/hw_device_ctx가 배선됨 — avcodec_open2 전에 init.
    // 실패하면 hw.active()==false 이고 dec는 그대로 SW 디코더로 열린다 (완전 폴백).
    HwContext hw;
    const bool hwReady = hw.init(dec, codec);
    std::fprintf(stderr, "[FfmpegStreamSource] decode path = %s\n", hwReady ? "HW (videotoolbox)" : "SW");
    const AVPixelFormat hwPix = hw.hwPixFmt();

    if (avcodec_open2(dec, codec, nullptr) < 0) {
        if (!m_stop) listener->onSourceError(DiagnosisReason::DecodeError);
        avcodec_free_context(&dec);
        m_graceUntilMs = steadyNowMs() + 1000;   // 읽기 중단 후 TEARDOWN 송신 유예 (2단계)
        avformat_close_input(&fmt);
        return;
    }

    if (!m_stop) listener->onSessionOpened();  // 세션 열림 ≠ 연결 확정 (D1)

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frm = av_frame_alloc();
    AVFrame* swf = av_frame_alloc();   // hw→cpu 전송 대상 (HW 경로에서만 사용)
    SwsContext* sws = nullptr;
    std::vector<uint8_t> rgba;
    bool gotKeyframe = false;   // 첫 키프레임(IDR) 전 패킷은 디코더에 주지 않는다 (HW 안전)

    while (!m_stop) {
        rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            // 타임아웃/EOF/네트워크 단절 — 패킷이 더 오지 않는다
            if (!m_stop) listener->onSourceError(DiagnosisReason::NoPackets);
            break;
        }
        if (pkt->stream_index != vIdx) {
            av_packet_unref(pkt);
            continue;
        }
        // HW 디코더(VideoToolbox)는 첫 패킷이 키프레임(IDR)이 아니면 디코드 세션이
        // 나쁜 상태에 빠져 avcodec_receive_frame이 영구 블로킹된다(CoreMedia 세마포어 대기).
        // 따라서 디코더에 첫 패킷을 보내기 전에 첫 키프레임까지 패킷을 버린다.
        // (SW 경로에도 무해 — 어차피 키프레임 전 P프레임은 표시 불가.)
        if (!gotKeyframe) {
            if ((pkt->flags & AV_PKT_FLAG_KEY) == 0) {
                av_packet_unref(pkt);
                continue;   // 키프레임 도착 전 — 디코더에 주지 않고 버린다
            }
            gotKeyframe = true;
        }
        if (!m_stop) listener->onPacketReceived();

        const int sendRc = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        // 손상 패킷/일시적 HW 디코드 실패(VideoToolbox 재구성 시 -12909 등)는 비치명 —
        // 한 패킷 버리고 계속한다(ffmpeg CLI 동작과 동일). EAGAIN은 정상 배압.
        // 진짜 치명 오류(코덱 닫힘 등)만 세션을 끝낸다.
        if (sendRc < 0 && sendRc != AVERROR(EAGAIN) && sendRc != AVERROR_INVALIDDATA) {
            if (!m_stop) listener->onSourceError(DiagnosisReason::DecodeError);
            break;
        }
        while (true) {
            const int recvRc = avcodec_receive_frame(dec, frm);
            if (recvRc == AVERROR(EAGAIN) || recvRc == AVERROR_EOF) break;  // 더 줄 프레임 없음
            if (recvRc < 0) {
                // 일시적 디코드 오류(HW 재구성 등) — 이 프레임만 건너뛰고 다음 패킷으로.
                av_frame_unref(frm);
                break;
            }
            if (!m_stop) listener->onFrameDecoded();

            // HW 경로: frm->format == hwPix이면 GPU 서피스. CPU 표시는 av_hwframe_transfer_data로
            // SW 프레임을 받아 RGBA로 변환한다. zero-copy GPU 직행은 Task5에서.
            const bool isHwFrame =
                hwReady && hwPix != AV_PIX_FMT_NONE &&
                static_cast<AVPixelFormat>(frm->format) == hwPix;

            // RGBA로 스케일할 원본 프레임 — HW면 transfer 결과(swf), SW면 frm 그대로.
            AVFrame* src = frm;
            void* gpuHandle = nullptr;

            if (isHwFrame) {
                gpuHandle = frm->data[3];   // VideoToolbox: CVPixelBufferRef
                av_frame_unref(swf);
                if (av_hwframe_transfer_data(swf, frm, 0) == 0) {
                    src = swf;
                } else {
                    // 전송 실패 — 이 프레임은 표시 불가, drop
                    av_frame_unref(frm);
                    continue;
                }
            }

            sws = sws_getCachedContext(sws, src->width, src->height,
                                       static_cast<AVPixelFormat>(src->format),
                                       src->width, src->height, AV_PIX_FMT_RGBA,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws != nullptr) {
                rgba.resize(static_cast<size_t>(src->width) * src->height * 4);
                uint8_t* dst[4] = {rgba.data(), nullptr, nullptr, nullptr};
                int dstStride[4] = {src->width * 4, 0, 0, 0};
                sws_scale(sws, src->data, src->linesize, 0, src->height, dst, dstStride);
                if (isHwFrame) {
                    // GPU 핸들 소유 + 폴백 표시용 RGBA 동반 발행 (Task5 전까지 SW 렌더러가 그림).
                    m_frameSlot.publishGpu(src->width, src->height, gpuHandle, rgba.data());
                } else {
                    m_frameSlot.publishCpu(src->width, src->height, rgba.data());
                }
            }
            if (isHwFrame) av_frame_unref(swf);
            av_frame_unref(frm);
        }
    }

    if (sws != nullptr) sws_freeContext(sws);
    av_frame_free(&swf);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    m_graceUntilMs = steadyNowMs() + 1000;   // 읽기 중단 후 TEARDOWN 송신 유예 (2단계)
    avformat_close_input(&fmt);
}

} // namespace nv::infra
