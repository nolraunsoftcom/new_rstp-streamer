#include "FfmpegStreamSource.h"
#include "FfmpegRecorder.h"
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
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
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
#elif defined(_WIN32)
// Windows GPU 핸들 = AVFrame*. D3D11VA 디코더는 텍스처 배열 풀을 재사용하므로 AVFrame ref(clone)를
// 잡아야 슬라이스 수명이 유지된다. retain=clone(새 ref), release=free(ref 반납).
void* frameRetain(void* p) {
    return av_frame_clone(static_cast<AVFrame*>(p));
}
void frameRelease(void* p) {
    AVFrame* f = static_cast<AVFrame*>(p);
    av_frame_free(&f);
}
#endif
} // namespace

FfmpegStreamSource::FfmpegStreamSource(LatestSurfaceSlot& frameSlot) : m_frameSlot(frameSlot) {
    avformat_network_init();
#if defined(__APPLE__)
    m_frameSlot.setGpuRefcounters(&cvRetain, &cvRelease);
#elif defined(_WIN32)
    m_frameSlot.setGpuRefcounters(&frameRetain, &frameRelease);
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
    m_stop = true;   // interrupt callback이 블로킹 호출을 깨운다

    if (!m_thread.joinable()) return;

    // close()가 디코드 스레드 자신에서 호출되면(재진입: run()→listener→disconnect→close)
    // self-join은 deadlock/terminate가 되므로 detach한다. 그 외에는 디코드 스레드가
    // 끝날 때까지 join한다. m_stop=true + interrupt 콜백으로 FFmpeg I/O가 곧 풀려 종료된다.
    //
    // (기존엔 별도 joiner 스레드 + 5초 타임아웃 후 m_thread.detach() 로직이었으나, joiner의
    //  m_thread.join()과 close()의 m_thread.detach()가 같은 std::thread에 동시 작용해 상태가
    //  깨지는 race가 있었다 — Windows에서 ucrtbase fail-fast(0xc0000409)의 원인. 단순화로 제거.)
    if (m_thread.get_id() == std::this_thread::get_id()) {
        m_thread.detach();
        return;
    }
    m_thread.join();
}

bool FfmpegStreamSource::startRecording(const std::string& outputPath) {
    if (outputPath.empty()) return false;
    // D1: 이미 녹화 중이어도 거절하지 않는다. 새 경로를 pending으로 갱신하고 요청 래치를
    // 올리면, 디코드 스레드의 serviceRecording이 pendingPath != currentPath를 감지해
    // 현재 세그먼트를 마감(finish)하고 새 경로로 재start한다. 즉 finish/start의 직렬 수행을
    // 디코드 스레드가 단독으로 소유 → control 스레드의 stop→start 래치 경합이 사라진다.
    {
        std::lock_guard lk(m_recMu);
        m_pendingPath = outputPath;
    }
    m_recordRequested.store(true);   // 디코드 스레드가 다음 키프레임에서 레코더를 연다/전환한다
    return true;
}

void FfmpegStreamSource::stopRecording() {
    // 요청 래치를 내리면 디코드 루프가 다음 반복에서 활성 레코더를 stop/소멸한다.
    m_recordRequested.store(false);
    {
        std::lock_guard lk(m_recMu);
        m_pendingPath.clear();
    }
}

// 디코드 스레드 전용. 녹화 요청 상태에 맞춰 레코더를 생성/start/stop하고,
// 활성 레코더가 있으면 현재 패킷을 기록한다. inputStream은 demux 중인 입력 비디오 stream.
void FfmpegStreamSource::serviceRecording(AVStream* inputStream, const AVPacket* pkt) {
    const bool wantRecord = m_recordRequested.load();

    // 중지 요청 또는 종료 중 — 활성 레코더 종료(세그먼트 종료).
    if (!wantRecord) {
        if (m_recorder) finishRecorder();
        return;
    }

    std::string path;
    {
        std::lock_guard lk(m_recMu);
        path = m_pendingPath;
    }
    if (path.empty()) return;

    // D1 세그먼트 전환: 레코더가 열려 있는데 요청 경로가 현재 경로와 다르면, 현재 세그먼트를
    // 마감(trailer)하고 새 경로로 다시 연다(다음 키프레임부터 새 세그먼트). control 스레드는
    // 경로만 바꿨고 실제 finish→start는 여기(디코드 스레드)에서 직렬로 일어난다 → 래치 경합 없음.
    if (m_recorder && path != m_currentPath) {
        finishRecorder();
    }

    // 시작 요청인데 아직 레코더가 없으면(최초 start 또는 전환 직후) — 입력 stream으로 새로 start.
    if (!m_recorder) {
        if (inputStream == nullptr) return;
        auto rec = std::make_unique<FfmpegRecorder>();
        if (!rec->start(path, inputStream)) {
            // 격리: 시작 실패는 이 채널 녹화만 포기. 디코드는 영향 없음.
            // 요청 래치를 내려 매 패킷 재시도하지 않도록 한다(폭주 방지).
            std::fprintf(stderr, "[FfmpegStreamSource] 녹화 시작 실패: %s\n", path.c_str());
            m_recordRequested.store(false);
            {
                std::lock_guard lk(m_recMu);
                m_pendingPath.clear();
            }
            return;
        }
        m_recorder = std::move(rec);
        m_currentPath = path;
        m_recording.store(true);
        m_recordError.store(false);   // D10: 새 세그먼트 — 이전 오류 미러 초기화
    }

    // 활성 레코더에 현재(디코더로 갈) 패킷을 기록 — FfmpegRecorder가 내부에서 av_packet_ref로
    // 복제하므로 원본 pkt는 변경되지 않고 디코더로 그대로 보낼 수 있다(화면=녹화 일치).
    if (m_recorder && pkt != nullptr) {
        m_recorder->writePacket(pkt);
        // D10: 쓰기 오류(디스크 풀 등)를 control 스레드가 읽을 수 있도록 atomic에 미러.
        // 디코드 루프는 계속 돈다(격리) — 가시화는 RecordingController reconcile가 담당.
        if (m_recorder->hadError()) m_recordError.store(true);
    }
}

// 활성 레코더 세그먼트를 종료한다(stop + 소멸). 디코드 스레드에서만 호출.
void FfmpegStreamSource::finishRecorder() {
    if (m_recorder) {
        m_recorder->stop();
        m_recorder.reset();
    }
    m_currentPath.clear();
    m_recording.store(false);
    m_recordError.store(false);   // D10: 세그먼트 종료 — 다음 레코더가 자기 상태를 새로 보고
}

void FfmpegStreamSource::run(std::string url, nv::app::StreamSourceListener* listener) {
  // 디코드 스레드 경계: 예외가 스레드 밖으로 새면 std::terminate(0xc0000409)로 앱이 죽는다.
  // 잘못된 채널/손상 스트림에서도 앱을 죽이지 않고 에러로 보고하도록 경계에서 잡는다.
  try {
    AVFormatContext* fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = &FfmpegStreamSource::interruptCb;
    fmt->interrupt_callback.opaque = this;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "rtsp,rtsps,rtp,tcp,udp,tls", 0);  // S1: 허용 프로토콜만 열거 (rtsps=RTSP-over-TLS, tls 필요)
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
    std::fprintf(stderr, "[FfmpegStreamSource] decode path = %s\n", hwReady ? "HW" : "SW");
    const AVPixelFormat hwPix = hw.hwPixFmt();
    // Windows: QRhi 공유 디바이스로 hw를 만들었으면 GPU 변환 zero-copy 경로(CPU 왕복 제거).
    // 아니면(자체 디바이스/macOS) 기존 CPU 변환 경로. (macOS에선 미사용.)
    [[maybe_unused]] const bool hwShared = hw.sharesRenderDevice();

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
        if (!m_stop) {
            listener->onPacketReceived();
            listener->onBytesReceived(pkt->size);   // 상태바 bitrate 산출용
        }

        // ── 패킷 fan-out (M3 Task3) ──────────────────────────────────────────
        // 디코더에 보내기 전에 레코더에도 같은 패킷을 전달한다(화면=녹화 일치).
        // serviceRecording은 녹화 요청 상태에 맞춰 레코더를 start/stop하고, 활성 시
        // pkt를 기록한다(내부에서 av_packet_ref 복제 — 원본 pkt 불변). 레코더 오류는
        // 격리되어 이 루프를 멈추지 않는다(D8).
        serviceRecording(fmt->streams[vIdx], pkt);

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

#if defined(_WIN32)
            // Windows zero-copy: 공유 디바이스 HW 프레임은 CPU 전송/sws 없이 AVFrame을 그대로
            // 발행한다(슬롯이 clone해 슬라이스 수명 유지). 렌더러 D3D11 브리지가 GPU에서
            // NV12→RGBA 변환. 동반 RGBA 없음(CPU 왕복 제거 — 부채 #15 Windows).
            if (isHwFrame && hwShared) {
                m_frameSlot.publishGpu(frm->width, frm->height, frm, /*rgba=*/nullptr);
                av_frame_unref(frm);
                continue;
            }
#endif
            // RGBA로 스케일할 원본 프레임 — HW면 transfer 결과(swf), SW면 frm 그대로.
            AVFrame* src = frm;
            void* gpuHandle = nullptr;

            if (isHwFrame) {
                gpuHandle = HwContext::extractGpuHandle(frm);   // R3: 플랫폼별 GPU 핸들
                av_frame_unref(swf);
                if (av_hwframe_transfer_data(swf, frm, 0) == 0) {
                    src = swf;
                } else {
                    // HW→CPU 전송 실패 — 이 프레임은 표시 불가, drop.
                    //
                    // ── 슬롯 핸들 불변량 (#10/#22) ─────────────────────────────────────
                    // publishGpu/publishCpu를 호출하지 않으므로 슬롯은 직전 성공 프레임의
                    // GPU 핸들(≤1개)을 그대로 유지한다. 이것은 의도된 동작이며 무해하다:
                    //   • LatestSurfaceSlot은 항상 "최신 유효 프레임 ≤1핸들"을 보유한다.
                    //     (publishGpu가 releaseGpuLocked를 먼저 호출해 이전 핸들을 해제)
                    //   • 소비자(렌더러)는 seq 가드로 이미 소비한 프레임을 재소비하지 않는다.
                    //     따라서 직전 핸들이 슬롯에 남아 있어도 이중 소비·이중 해제는
                    //     발생하지 않는다.
                    //   • gpuHandle은 이 분기에서 extractGpuHandle로 얻었지만 publishGpu에
                    //     전달되지 않으므로 슬롯이 retain하지 않는다. 핸들은 frm과 함께
                    //     av_frame_unref(frm)로 CVPixelBuffer 수명이 코덱 내부에서 관리된다.
                    //     (이 함수는 핸들을 별도 retain하지 않으므로 누수 없음.)
                    // ────────────────────────────────────────────────────────────────────
                    if (!m_stop) listener->onFrameDropped();   // 상태바 Dropped 카운트
                    av_frame_unref(frm);
                    continue;
                }
            }

            // 방어: 비정상 치수(손상 스트림/정수 오버플로)면 이 프레임을 버린다 — width*height*4
            // 가 거대해지면 rgba.resize가 bad_alloc을 던져 디코드 스레드가 재시작된다. 상한은
            // 8K(7680x4320)를 여유 있게 넘긴 8192로 둔다(정상 카메라 해상도는 모두 통과).
            if (src->width <= 0 || src->height <= 0 || src->width > 8192 || src->height > 8192) {
                if (!m_stop) listener->onFrameDropped();
                if (isHwFrame) av_frame_unref(swf);
                av_frame_unref(frm);
                continue;
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

    // 소스가 닫히거나 재open(재연결)으로 이 demux가 끝나면 활성 레코더 세그먼트를 종료한다.
    // (트레일러를 써서 MKV를 재생 가능 상태로 마감 — 재연결 후 RecordingController가
    //  다시 startRecording하면 새 세그먼트가 된다. M3 설계: 재연결 시 세그먼트 분리.)
    finishRecorder();

    if (sws != nullptr) sws_freeContext(sws);
    av_frame_free(&swf);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    m_graceUntilMs = steadyNowMs() + 1000;   // 읽기 중단 후 TEARDOWN 송신 유예 (2단계)
    avformat_close_input(&fmt);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[FfmpegStreamSource] decode thread threw: %s\n", e.what());
    if (!m_stop) listener->onSourceError(DiagnosisReason::DecodeError);
  } catch (...) {
    std::fprintf(stderr, "[FfmpegStreamSource] decode thread threw (unknown)\n");
    if (!m_stop) listener->onSourceError(DiagnosisReason::DecodeError);
  }
}

} // namespace nv::infra
