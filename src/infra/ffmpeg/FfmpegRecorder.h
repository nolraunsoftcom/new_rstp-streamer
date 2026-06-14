#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include <libavutil/avutil.h>   // AV_NOPTS_VALUE
#include <libavutil/rational.h>
}

struct AVStream;
struct AVPacket;
struct AVFormatContext;
struct AVCodecParameters;

namespace nv::infra {

// 단일 파일 MKV remux 라이터 (설계 D7 크래시 내성 · D8 녹화-재생 격리).
//
// 디코딩 없는 stream copy: 입력 비디오 stream의 codecpar/timebase를 받아 Matroska로
// 그대로 mux한다. 화면에 보이는 바로 그 패킷을 기록하므로 화면=녹화 바이트 일치.
//
// 경계: 이 클래스는 app 포트(IRecordingSink)를 구현하지 않는다 — IRecordingSink는
// channelId/path 수준의 app 명령이고, FfmpegRecorder는 AVPacket/AVStream을 직접 다루는
// infra 내부 협업이다. 배선은 infra(FfmpegStreamSource→FfmpegRecorder, Task3)에서.
//
// 격리: 모든 FFmpeg 오류는 false 반환 + 내부 플래그로 처리하고 예외를 던지지 않는다.
// 디코드 스레드(writePacket 호출측)로 오류가 전파되지 않는다 — 레코더는 자기 상태만 잃는다.
//
// 세그먼트 롤오버: 이 클래스는 단일 파일 remux만 책임진다. 롤오버는 호출측
// (RecordingController, Task4)이 stop() 후 새 경로로 start()를 호출해 구현한다.
//
// 스레드(D8 비동기 쓰기 큐): writePacket은 디코드 스레드에서 호출되며, rescale/오프셋만
// 적용한 패킷 복사본을 유한 큐에 넣고 즉시 반환한다 — 블로킹 디스크 I/O
// (av_interleaved_write_frame)는 전용 쓰기 스레드가 수행한다. 느린 저장소(USB HDD/네트워크
// 마운트)에서도 디코드 핫루프가 막히지 않는다.
//
// 큐 유한·드랍 정책: 큐가 가득 차면 가장 오래된 비키프레임 패킷을 드랍한다(GOP 단위 — 유인·
// 단기 녹화라 드랍 수용). 드랍 수를 카운트해 stop 시 1회 로그한다. 키프레임은 보존하려
// 노력하되 큐가 키프레임으로만 차면(degenerate) 가장 오래된 것을 드랍한다.
//
// 격리(D8): 쓰기 스레드의 mux 실패는 m_errored(atomic)만 세우고 예외를 던지지 않는다 —
// 디코드 스레드(writePacket)로 전파되지 않는다. 디코드는 계속 큐에 넣되 hadError()로 진단.
//
// 수명/스레드 안전: start()가 쓰기 스레드를 띄우고, stop()이 큐를 flush한 뒤 스레드를 join하고
// 트레일러를 쓴다. 패킷 소유권은 큐가 가진다(enqueue 시 이전된 AVPacket*를 쓰기 스레드가
// av_packet_free). writePacket/start/stop은 한 스레드(디코드)에서만 호출한다 — 큐 뮤텍스가
// 디코드↔쓰기 스레드 간 패킷 핸드오프를 보호한다.
class FfmpegRecorder {
public:
    FfmpegRecorder() = default;
    ~FfmpegRecorder();

    FfmpegRecorder(const FfmpegRecorder&) = delete;
    FfmpegRecorder& operator=(const FfmpegRecorder&) = delete;

    // MKV 출력을 연다. inputVideoStream의 codecpar/time_base로 출력 stream을 만든다.
    // 성공 시 true(이후 writePacket 가능), 실패 시 false + 부분 자원 완전 정리.
    // 이미 녹화 중이면(중복 start) false.
    bool start(const std::string& path, const AVStream* inputVideoStream);

    // 입력 패킷 하나를 기록한다. 첫 키프레임(AV_PKT_FLAG_KEY) 전 패킷은 드랍한다.
    // pts/dts/duration을 입력 timebase→출력 timebase로 rescale하고 첫 키프레임 pts를
    // 0 기준으로 오프셋한다(세그먼트가 0부터 시작). 원본 pkt는 변경하지 않는다(복사본 사용).
    // 미시작/오류 상태에서는 아무것도 하지 않는다.
    void writePacket(const AVPacket* pkt);

    // 트레일러를 쓰고 출력을 닫는다. 재호출/미시작 안전(멱등).
    void stop();

    bool isRecording() const { return m_open; }

    // 오류 플래그가 한 번이라도 섰는지(격리 진단용). start 성공으로 리셋.
    // 쓰기 스레드가 set하고 디코드 스레드가 read하므로 atomic.
    bool hadError() const { return m_errored.load(); }

private:
    void cleanup();        // 부분/완전 자원 해제 (멱등) — 쓰기 스레드 종료 후에만 호출
    void writerLoop();     // 전용 쓰기 스레드: 큐에서 패킷을 꺼내 mux (블로킹 I/O 격리)
    void stopWriter();     // 쓰기 스레드에 종료 신호 + join (큐 flush는 호출 전 보장)
    void dropOldestNonKeyLocked();  // 큐 가득 시 가장 오래된 비키프레임 드랍 (m_qMu 보유 가정)

    AVFormatContext* m_fmt = nullptr;
    int m_outStreamIndex = -1;
    AVRational m_inTimeBase{0, 1};   // 입력 stream time_base 보관(rescale용)
    std::string m_path;              // 지연 avio_open을 위한 출력 경로 보관
    bool m_open = false;
    bool m_headerWritten = false;    // avio_open+write_header가 실제로 수행됐는지(첫 키프레임 시점)
    bool m_gotKeyframe = false;
    std::atomic<bool> m_errored{false};
    int64_t m_firstPts = AV_NOPTS_VALUE;   // 첫 키프레임 pts(입력 timebase) — 0 오프셋 기준

    // ── D8 비동기 쓰기 큐 ────────────────────────────────────────────────────
    // 큐 상한: 30fps 기준 ~2초 분량. 느린 저장소에서 디코드 핫루프 보호 + 메모리 상한.
    static constexpr size_t kMaxQueue = 60;
    std::thread             m_writer;
    std::mutex              m_qMu;
    std::condition_variable m_qCv;
    std::deque<AVPacket*>   m_queue;        // 쓰기 대기 패킷(소유 — 쓰기 스레드가 free)
    bool                    m_writerStop = false;  // m_qMu 보호: stop 신호
    uint64_t                m_droppedPackets = 0;  // m_qMu 보호: 큐 가득으로 드랍한 패킷 수
};

} // namespace nv::infra
