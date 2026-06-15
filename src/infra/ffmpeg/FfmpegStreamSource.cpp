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
#include <condition_variable>
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
    m_stop = true;   // interrupt callback이 블로킹 호출을 깨운다

    if (!m_thread.joinable()) return;

    // VideoToolbox avcodec_receive_frame가 CoreMedia 세마포어에서 wedge되면 join이
    // 영원히 막혀 control 스레드 전체가 정지한다.
    //
    // 안전 전략: shared 상태를 힙에 두고 별도 joiner 스레드가 join을 시도한다.
    // 5초 이내에 join 완료되면 정상 경로(joiner를 join해 끝).
    // 타임아웃이면 디코드 스레드를 detach하고 joiner도 detach — close() ≤5s 보장.
    //
    // shared_ptr 이유: 타임아웃 경로에서 close()가 먼저 반환된 뒤 joiner 스레드가
    // 계속 실행될 수 있다. 스택 변수를 캡처하면 dangling 참조 UB 발생하므로
    // 공유 상태는 모두 힙에 둔다.
    //
    // detach된 디코드 스레드는 m_stop=true라 I/O interrupt / 다음 패킷 타임아웃으로
    // 결국 빠져나오고, 그때 joiner 스레드의 join()도 완료되어 joiner도 종료된다.
    // 앱 관리 응답성 보존: wedge 시 누수보다 영구 블로킹이 더 치명적.
    struct JoinState {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
    };
    auto state = std::make_shared<JoinState>();

    std::thread joiner([this, state] {
        m_thread.join();
        {
            std::lock_guard lk(state->mu);
            state->done = true;
        }
        state->cv.notify_one();
    });

    bool joinedInTime = false;
    {
        std::unique_lock lk(state->mu);
        joinedInTime = state->cv.wait_for(lk, std::chrono::seconds(5),
                                          [&state] { return state->done; });
    }

    if (joinedInTime) {
        joiner.join();   // 정상 경로: joiner는 이미 완료 상태
    } else {
        // 5초 타임아웃 — 디코드 스레드가 wedge됐다고 판단.
        std::fprintf(stderr,
            "[FfmpegStreamSource] CRITICAL: decode thread wedged — detaching after 5s\n");
        // joiner를 먼저 detach해야 한다: 이후 m_thread.detach() 하면 joiner의
        // m_thread.join() 호출이 not-joinable 스레드에 대한 것이 되어 terminate.
        // joiner가 detach된 뒤 m_thread를 detach하면 joiner는 잠시 후 join()이
        // 반환(detach된 스레드 대상 join은 UB)되므로 순서를 바꾼다:
        // m_thread.detach()를 먼저 하면 joiner의 join()이 UB — 안전하지 않다.
        //
        // 올바른 순서: joiner.detach() 먼저 → m_thread.detach().
        // joiner가 detach된 상태에서 joiner 내부의 m_thread.join()이 실행되면
        // m_thread가 아직 joinable이므로 정상 join된다. m_thread는 m_stop=true라
        // 곧 종료되고 joiner도 종료된다. shared_ptr(state)는 joiner 종료 시 소멸.
        joiner.detach();
        m_thread.detach();
    }
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
    std::fprintf(stderr, "[ffsrc] run() enter url=%s\n", url.c_str());
    AVFormatContext* fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = &FfmpegStreamSource::interruptCb;
    fmt->interrupt_callback.opaque = this;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "rtsp,rtsps,rtp,tcp,udp,tls", 0);  // S1: 허용 프로토콜만 열거 (rtsps=RTSP-over-TLS, tls 필요)
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);   // R4: 로컬 레그 TCP 강제
    av_dict_set(&opts, "timeout", "5000000", 0);      // 소켓 타임아웃 5s(us) — FFmpeg 5+
    av_dict_set(&opts, "stimeout", "5000000", 0);     // 구버전 옵션명 (무시돼도 무해)

    std::fprintf(stderr, "[ffsrc] avformat_open_input start\n");
    int rc = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
    std::fprintf(stderr, "[ffsrc] avformat_open_input rc=%d\n", rc);
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
