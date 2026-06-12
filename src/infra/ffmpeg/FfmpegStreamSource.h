#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "src/app/ports/IStreamSource.h"
#include "src/infra/video/LatestSurfaceSlot.h"

// FFmpeg 타입은 전역 스코프에 forward 선언(FfmpegRecorder.h 선례). 헤더에 FFmpeg
// 헤더를 끌어들이지 않기 위함 — 정의는 .cpp에서 extern "C" 블록으로 포함한다.
struct AVStream;
struct AVPacket;

namespace nv::infra {

class FfmpegRecorder;

// IStreamSource의 FFmpeg 구현 (설계 D4).
// open(): demux+decode 스레드를 띄운다. 리스너 콜백은 그 스레드에서 호출되므로
// 호출측은 MarshallingStreamSource로 감싸 control 스레드로 직렬화해야 한다.
// RTSP 전송은 TCP 강제 (설계 3차 리뷰 R4).
// M2b: open에서 VideoToolbox HW 디코딩을 시도하고 실패 시 기존 SW 경로로 완전 폴백한다.
class FfmpegStreamSource final : public nv::app::IStreamSource {
public:
    explicit FfmpegStreamSource(LatestSurfaceSlot& frameSlot);
    ~FfmpegStreamSource() override;

    void open(const std::string& url, nv::app::StreamSourceListener& listener) override;
    void close() override;   // demux 스레드 합류까지 블로킹

    // ── 녹화 fan-out (M3 Task3) ──────────────────────────────────────────────
    // 녹화 명령은 control 스레드, demux는 디코드 스레드에서 동작한다 → 스레드 안전.
    // 시작/중지는 "요청 플래그"만 세우고, 실제 FfmpegRecorder 생성/start/stop은
    // 디코드 스레드(run 루프)가 입력 비디오 stream(codecpar/timebase)이 살아있는 동안
    // 수행한다. 입력 AVStream은 AVFormatContext 소유라 디코드 스레드에서만 접근 가능 —
    // 따라서 레코더 수명을 그 스레드에 묶어 락 없이 안전하게 다룬다.
    //
    // 화면=녹화 일치: demux 루프가 av_read_frame으로 얻은 바로 그 패킷을 디코더에 보내기
    // 전에 av_packet_ref로 복제해 레코더에 전달한다(같은 산출 패킷).
    //
    // 격리(D8): 레코더 writePacket 실패는 그 레코더만 오류 상태로 둘 뿐 디코드 루프를
    // 멈추지 않는다(FfmpegRecorder가 예외를 던지지 않음).

    // 녹화 시작/세그먼트 전환 요청. 다음 키프레임부터 outputPath로 기록한다.
    // 이미 녹화 중이어도 거절하지 않고 새 경로를 pending으로 받아 디코드 스레드가
    // serviceRecording에서 현재 세그먼트를 마감(finish)하고 새 경로로 재start한다
    // (전환을 디코드 스레드 단일 소유로 — control 스레드 stop→start 래치 경합 제거, D1).
    // outputPath가 비면 false.
    bool startRecording(const std::string& outputPath);
    // 녹화 중지 요청. 디코드 루프가 다음 반복에서 레코더를 stop/소멸한다.
    void stopRecording();
    // 현재 실제로 녹화 중인지(디코드 스레드가 레코더를 열어둔 상태인지) 반환.
    bool isRecording() const { return m_recording.load(); }

private:
    void run(std::string url, nv::app::StreamSourceListener* listener);
    static int interruptCb(void* opaque);

    // 디코드 스레드 전용: 녹화 요청을 반영해 레코더를 생성/start/stop한다.
    // inputStream은 현재 demux 중인 입력 비디오 stream(레코더 start에 필요).
    void serviceRecording(AVStream* inputStream, const AVPacket* pkt);
    // 소스 close/재open 시 활성 레코더 세그먼트를 종료한다(디코드 스레드에서 호출).
    void finishRecorder();

    LatestSurfaceSlot& m_frameSlot;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<int64_t> m_graceUntilMs{0};  // close 중 TEARDOWN 송신 허용 데드라인 (steady ms)

    // 녹화 명령 채널 (control 스레드 set, 디코드 스레드 read).
    std::mutex m_recMu;                         // m_pendingPath 보호
    std::string m_pendingPath;                  // startRecording이 채움(빈 문자열=요청 없음)
    std::atomic<bool> m_recordRequested{false}; // 시작 요청 래치
    std::atomic<bool> m_recording{false};       // 디코드 스레드가 레코더를 연 상태
    std::unique_ptr<FfmpegRecorder> m_recorder; // 디코드 스레드 전용 소유(락 불필요)
    std::string m_currentPath;                  // 디코드 스레드 전용: 현재 레코더가 쓰는 경로
                                                // (m_pendingPath와 다르면 세그먼트 전환, D1)
};

} // namespace nv::infra
