#include "RecordingController.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

// 순수 app 레이어 — Qt/FFmpeg include 없음.
// 경로 생성은 C++ stdlib(chrono + ctime + sstream)만 사용.
namespace nv::app {

// 채널명에서 파일명에 안전하지 않은 문자를 '_'로 치환한다.
static std::string sanitizeName(const std::string& name) {
    std::string s = name.empty() ? "channel" : name;
    for (char& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' ||
            static_cast<unsigned char>(c) < 0x20) {
            c = '_';
        }
    }
    return s;
}

RecordingController::RecordingController(IRecordingSink& sink,
                                         IClock& clock,
                                         ILogger& logger,
                                         nv::domain::SegmentPolicy policy)
    : m_sink(sink), m_clock(clock), m_logger(logger), m_policy(policy) {}

void RecordingController::setObserver(RecordingObserver observer) {
    m_observer = std::move(observer);
}

nv::domain::RecordingState RecordingController::stateOf(const std::string& channelId) const {
    auto it = m_channels.find(channelId);
    return it == m_channels.end() ? nv::domain::RecordingState::Idle : it->second.state;
}

// ── 내부 헬퍼 ───────────────────────────────────────────────────────────────

std::string RecordingController::makePath(const std::string& channelName) const {
    // 타임스탬프: chrono time_point → time_t → localtime → strftime
    const auto tp = m_clock.now();
    // steady_clock은 time_t 변환 불가 → system_clock 기준 오프셋으로 조회.
    // (steady_clock epoch는 구현 정의라 time_t 변환 불가 — system_clock 사용.)
    (void)tp;  // makePath는 system_clock을 직접 호출해 타임스탬프를 생성
    const std::time_t t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());

    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts[20];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_buf);

    // D2 충돌 방지: 같은 초 안의 stop→start(롤오버/재연결)는 동일 타임스탬프라 경로가
    // 겹쳐 avio_open이 직전 세그먼트를 0바이트로 truncate한다. 단조 증가 시퀀스를 접미사로
    // 붙여 같은 초라도 항상 다른 경로를 보장한다(..._HHmmss_NNN). 시퀀스는 컨트롤러 수명 동안
    // 단조 증가 — control 스레드 단일 호출이라 atomic 불필요(단순 멤버).
    const unsigned seq = m_pathSeq++;
    char seqbuf[8];
    std::snprintf(seqbuf, sizeof(seqbuf), "_%03u", seq % 1000);

    // 저장 디렉토리: 환경변수 NV_RECORD_DIR 또는 빈 문자열(cwd 상대).
    // 실제 앱은 infra가 main()에서 NV_RECORD_DIR을 QStandardPaths로 채워 줘도 되고,
    // ChannelSourceFactory::startRecording이 경로를 덮어써도 된다.
    // 테스트에서는 /tmp 또는 빈 디렉토리 사용.
    std::string dir;
    if (const char* env = std::getenv("NV_RECORD_DIR")) {
        dir = env;
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    }

    return dir + sanitizeName(channelName) + '_' + ts + seqbuf + ".mkv";
}

void RecordingController::notify(const std::string& channelId, nv::domain::RecordingState s) {
    if (m_observer) m_observer(channelId, s);
}

void RecordingController::doStart(const std::string& channelId, const std::string& channelName) {
    const std::string path = makePath(channelName);
    const bool ok = m_sink.startRecording(channelId, path);
    if (!ok) {
        // D8 격리: 이 채널만 Idle 유지, 경고 로그. 다른 채널·재생 무영향.
        m_logger.log(LogLevel::Warn, channelId, "RecordingController",
                     "startRecording 실패 — Idle 유지: " + path);
        auto& ch = m_channels[channelId];
        ch.state = nv::domain::RecordingState::Idle;
        ch.channelName = channelName;
        notify(channelId, nv::domain::RecordingState::Idle);
        return;
    }
    auto& ch = m_channels[channelId];
    ch.state        = nv::domain::RecordingState::Recording;
    ch.channelName  = channelName;
    ch.segmentStart = m_clock.now();
    notify(channelId, nv::domain::RecordingState::Recording);
}

void RecordingController::doStop(const std::string& channelId) {
    m_sink.stopRecording(channelId);
    auto& ch  = m_channels[channelId];
    ch.state  = nv::domain::RecordingState::Idle;
    notify(channelId, nv::domain::RecordingState::Idle);
}

// ── 공개 API ─────────────────────────────────────────────────────────────────

void RecordingController::toggle(const std::string& channelId,
                                 const std::string& channelName) {
    auto it = m_channels.find(channelId);
    const bool active = (it != m_channels.end()) &&
        (it->second.armed ||
         it->second.state == nv::domain::RecordingState::Recording);
    if (active) {
        // 녹화 의도 해제 + 현재 세그먼트 종료
        m_channels[channelId].armed = false;
        doStop(channelId);
    } else {
        // 녹화 의도 설정 + 첫 세그먼트 즉시 시작(즉각적 시작감)
        m_channels[channelId].armed = true;
        doStart(channelId, channelName);
    }
}

void RecordingController::onChannelRemoved(const std::string& channelId) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return;
    it->second.armed = false;
    if (it->second.state == nv::domain::RecordingState::Recording) {
        // best-effort: 소스가 곧 파괴되므로 실패 가능, 유령 상태 방지가 핵심
        m_sink.stopRecording(channelId);
        notify(channelId, nv::domain::RecordingState::Idle);
    }
    m_channels.erase(it);
}

void RecordingController::onReconnect(const std::string& channelId,
                                      const std::string& /*channelName*/) {
    if (!m_policy.splitOnReconnect) return;
    auto it = m_channels.find(channelId);
    if (it == m_channels.end() || !it->second.armed ||
        it->second.state != nv::domain::RecordingState::Recording) return;

    // 드롭 엣지: 죽어가는 소스에 doStart 하지 않는다. 현재 세그먼트만 종료하고
    // armed는 유지 — 복구(onStreaming) 시 새 세그먼트가 시작된다.
    doStop(channelId);
}

void RecordingController::onStreaming(const std::string& channelId,
                                      const std::string& channelName) {
    auto it = m_channels.find(channelId);
    // armed가 아니면(사용자가 녹화 의도 없음) 무동작
    if (it == m_channels.end() || !it->second.armed) return;
    // 이미 녹화 중이면 중복 시작 방지
    if (it->second.state == nv::domain::RecordingState::Recording) return;

    // armed인데 미녹화(드롭으로 종료됐거나 수렴으로 떨어짐) → 새 세그먼트 시작
    doStart(channelId, channelName);
}

void RecordingController::tick() {
    const auto now = m_clock.now();
    for (auto& [id, ch] : m_channels) {
        if (ch.state != nv::domain::RecordingState::Recording) continue;

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(now - ch.segmentStart);

        // D3 수렴(reconciliation): sink.startRecording 성공은 래치 설정일 뿐,
        // 디코드 스레드의 FfmpegRecorder::start가 실패하면 sink는 녹화를 닫고
        // isRecording==false가 되지만 컨트롤러는 영원히 Recording이라 UI REC 뱃지가
        // 거짓으로 남는다. 컨트롤러가 Recording인데 sink가 더 이상 녹화 중이 아니면
        // Idle로 수렴 + 경고 로그 + 옵저버 통지(UI REC 해제)한다.
        //
        // 유예(kReconcileGrace): 디코드 스레드는 다음 키프레임에서야 레코더를 연다 →
        // start 직후 첫 tick에는 sink.isRecording이 아직 false일 수 있다(정상 비동기 지연).
        // 유예 시간 안에는 수렴하지 않아 거짓 해제를 막는다. 유예 후에도 false면 진짜 실패.
        static constexpr std::chrono::seconds kReconcileGrace{3};
        if (elapsed >= kReconcileGrace && !m_sink.isRecording(id)) {
            m_logger.log(LogLevel::Warn, id, "RecordingController",
                         "녹화 중이라 표시됐으나 sink는 비녹화 — Idle로 수렴(REC 해제)");
            // H2: stopRecording을 명시적으로 호출해 FfmpegStreamSource의
            // m_recordRequested 래치를 확실히 내린다. wedge-detach 등 예외 경로에서
            // 좀비 녹화(레코더는 닫혔으나 래치가 살아있는 상태)를 차단한다.
            // armed는 유지 — H1 적용 후 여기 도달은 진짜 시작 실패이므로
            // 다음 onStreaming 시 재시도가 맞다(로그로 가시화됨).
            m_sink.stopRecording(id);
            ch.state = nv::domain::RecordingState::Idle;
            notify(id, nv::domain::RecordingState::Idle);
            continue;
        }

        if (elapsed >= m_policy.maxDuration) {
            // 롤오버: stop + 새 경로로 start
            m_sink.stopRecording(id);
            ch.state = nv::domain::RecordingState::Idle;  // 임시 — doStart가 Recording으로 전환
            doStart(id, ch.channelName);
        }
    }
}

} // namespace nv::app
