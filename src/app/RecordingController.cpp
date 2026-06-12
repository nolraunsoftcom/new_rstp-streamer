#include "RecordingController.h"

#include <chrono>
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
    // 테스트에서는 경로 내용보다 비어있지 않음만 검증하므로 실시간 대신
    // 단조 카운터를 사용해도 무방하지만, 실용성을 위해 system_clock을 직접 쓴다.
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

    // 저장 디렉토리: 환경변수 NV_RECORD_DIR 또는 빈 문자열(cwd 상대).
    // 실제 앱은 infra가 main()에서 NV_RECORD_DIR을 QStandardPaths로 채워 줘도 되고,
    // ChannelSourceFactory::startRecording이 경로를 덮어써도 된다.
    // 테스트에서는 /tmp 또는 빈 디렉토리 사용.
    std::string dir;
    if (const char* env = std::getenv("NV_RECORD_DIR")) {
        dir = env;
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    }

    return dir + sanitizeName(channelName) + '_' + ts + ".mkv";
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
    const auto state = stateOf(channelId);
    if (state == nv::domain::RecordingState::Idle) {
        doStart(channelId, channelName);
    } else {
        doStop(channelId);
    }
}

void RecordingController::onChannelRemoved(const std::string& channelId) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return;
    if (it->second.state == nv::domain::RecordingState::Recording) {
        // best-effort: 소스가 곧 파괴되므로 실패 가능, 유령 상태 방지가 핵심
        m_sink.stopRecording(channelId);
        notify(channelId, nv::domain::RecordingState::Idle);
    }
    m_channels.erase(it);
}

void RecordingController::onReconnect(const std::string& channelId,
                                      const std::string& channelName) {
    if (!m_policy.splitOnReconnect) return;
    auto it = m_channels.find(channelId);
    if (it == m_channels.end() ||
        it->second.state != nv::domain::RecordingState::Recording) return;

    // 현재 세그먼트 중지 후 새 세그먼트 시작
    m_sink.stopRecording(channelId);
    it->second.state = nv::domain::RecordingState::Idle;  // 임시 — doStart가 Recording으로 전환
    doStart(channelId, channelName);
}

void RecordingController::tick() {
    const auto now = m_clock.now();
    for (auto& [id, ch] : m_channels) {
        if (ch.state != nv::domain::RecordingState::Recording) continue;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(now - ch.segmentStart);
        if (elapsed >= m_policy.maxDuration) {
            // 롤오버: stop + 새 경로로 start
            m_sink.stopRecording(id);
            ch.state = nv::domain::RecordingState::Idle;  // 임시 — doStart가 Recording으로 전환
            doStart(id, ch.channelName);
        }
    }
}

} // namespace nv::app
