#pragma once
#include <chrono>
#include <optional>
#include "Policies.h"
#include "src/domain/health/DiagnosisReason.h"

namespace nv::domain {

enum class ConnState { Idle, Connecting, SessionOpen, Streaming, Stalled, Reconnecting, Failed };

constexpr std::string_view toString(ConnState s) {
    switch (s) {
        case ConnState::Idle:         return "Idle";
        case ConnState::Connecting:   return "Connecting";
        case ConnState::SessionOpen:  return "SessionOpen";
        case ConnState::Streaming:    return "Streaming";
        case ConnState::Stalled:      return "Stalled";
        case ConnState::Reconnecting: return "Reconnecting";
        case ConnState::Failed:       return "Failed";
    }
    return "Unknown";
}

// 상태머신이 호출자(앱 계층)에 지시하는 부수효과. 상태머신 자신은 아무것도 실행하지 않는다.
enum class Action { None, OpenSource, CloseSource };

struct Transition {
    Action action = Action::None;
};

// 설계 D1/D2. 시간은 절대 조회하지 않는다 — 모든 입력이 now를 받는다.
class ConnectionStateMachine {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    ConnectionStateMachine(ReconnectPolicy reconnect, StallPolicy stall);

    // 사용자 명령 (새 사이클 — 카운터 리셋 허용)
    Transition connectRequested(TimePoint now);
    Transition disconnectRequested(TimePoint now);
    Transition retryRequested(TimePoint now);   // Failed에서만 의미. 그 외엔 무시.

    // 외부 신호: relay 헬스체크가 소스 복귀를 감지 (M4에서 배선).
    // Failed에서만 즉시 재접속을 트리거하고, 다른 상태에선 무시한다 (D1).
    Transition sourceAvailableHint(TimePoint now);

    // 어댑터 이벤트
    Transition sessionOpened(TimePoint now);
    Transition packetReceived(TimePoint now);
    Transition framePresented(TimePoint now);
    Transition errorOccurred(DiagnosisReason reason, TimePoint now);

    // 주기 입력 (1초 주기 권장)
    Transition tick(TimePoint now);

    ConnState state() const { return m_state; }
    int reconnectAttempts() const { return m_attempts; }
    DiagnosisReason lastReason() const { return m_lastReason; }

private:
    Transition beginRetryOrFail(DiagnosisReason reason, ConnState retryState, TimePoint now);

    ReconnectPolicy m_reconnect;
    StallPolicy m_stall;
    ConnState m_state = ConnState::Idle;
    int m_attempts = 0;
    DiagnosisReason m_lastReason = DiagnosisReason::None;
    std::optional<TimePoint> m_dataDeadline;   // SessionOpen: 첫 패킷 데드라인
    std::optional<TimePoint> m_lastPacketAt;   // Streaming: stall 감지용
    std::optional<TimePoint> m_retryAt;        // Reconnecting/Stalled/Failed: 재시도 시각
};

} // namespace nv::domain
