#include "ConnectionStateMachine.h"

namespace nv::domain {

ConnectionStateMachine::ConnectionStateMachine(ReconnectPolicy reconnect, StallPolicy stall)
    : m_reconnect(reconnect), m_stall(stall) {}

Transition ConnectionStateMachine::connectRequested(TimePoint now) {
    (void)now;
    m_attempts = 0;
    m_lastReason = DiagnosisReason::None;
    m_dataDeadline.reset();
    m_lastPacketAt.reset();
    m_retryAt.reset();
    m_state = ConnState::Connecting;
    return {Action::OpenSource};
}

Transition ConnectionStateMachine::disconnectRequested(TimePoint now) {
    (void)now;
    m_attempts = 0;
    m_lastReason = DiagnosisReason::None;
    m_dataDeadline.reset();
    m_lastPacketAt.reset();
    m_retryAt.reset();
    m_state = ConnState::Idle;
    return {Action::CloseSource};
}

Transition ConnectionStateMachine::retryRequested(TimePoint now) {
    if (m_state != ConnState::Failed) return {};
    return connectRequested(now);
}

Transition ConnectionStateMachine::sourceAvailableHint(TimePoint now) {
    (void)now;
    if (m_state != ConnState::Failed) return {};  // 정상 재시도 사이클은 영향받지 않는다
    // 주의: m_attempts는 리셋하지 않는다. 이 probe가 실패하면 즉시 Failed로 복귀한다.
    // 카운터 리셋은 framePresented(자동) 또는 retryRequested(수동)뿐이다 (D2).
    m_state = ConnState::Connecting;
    m_retryAt.reset();
    return {Action::OpenSource};
}

Transition ConnectionStateMachine::sessionOpened(TimePoint now) {
    if (m_state != ConnState::Connecting) return {};
    m_state = ConnState::SessionOpen;
    m_dataDeadline = now + m_stall.dataConfirmTimeout;
    return {};
}

Transition ConnectionStateMachine::packetReceived(TimePoint now) {
    if (m_state == ConnState::SessionOpen) {
        m_state = ConnState::Streaming;
        m_dataDeadline.reset();
    }
    if (m_state == ConnState::Streaming) {
        m_lastPacketAt = now;
    }
    return {};
}

Transition ConnectionStateMachine::framePresented(TimePoint now) {
    (void)now;
    if (m_state == ConnState::Streaming) {
        m_attempts = 0;                      // D2: 리셋의 유일한 자동 지점
        m_lastReason = DiagnosisReason::None;
    }
    return {};
}

// 실패 공통 처리: 한도 내면 retryState(Reconnecting 또는 Stalled)로, 초과면 Failed(저빈도 모드).
Transition ConnectionStateMachine::beginRetryOrFail(DiagnosisReason reason, ConnState retryState,
                                                    TimePoint now) {
    m_lastReason = reason;
    m_dataDeadline.reset();
    m_lastPacketAt.reset();
    ++m_attempts;
    if (m_attempts > m_reconnect.maxAttempts) {
        // D1(3차 리뷰): Failed = 고속 재시도 중지 + 저빈도 재시도 모드. 영구 정지가 아니다.
        m_state = ConnState::Failed;
        m_lastReason = DiagnosisReason::GaveUp;
        m_retryAt = now + m_reconnect.slowRetryDelay;
        return {Action::CloseSource};
    }
    m_state = retryState;
    m_retryAt = now + m_reconnect.retryDelay;
    return {Action::CloseSource};
}

Transition ConnectionStateMachine::errorOccurred(DiagnosisReason reason, TimePoint now) {
    switch (m_state) {
        case ConnState::Connecting:
        case ConnState::SessionOpen:
            return beginRetryOrFail(reason, ConnState::Reconnecting, now);
        case ConnState::Streaming:
            return beginRetryOrFail(reason, ConnState::Stalled, now);
        default:
            return {};  // Idle/Failed/재시도 대기 중 늦게 도착한 에러는 무시
    }
}

Transition ConnectionStateMachine::tick(TimePoint now) {
    switch (m_state) {
        case ConnState::SessionOpen:
            if (m_dataDeadline && now >= *m_dataDeadline) {
                // 가짜 연결: 세션은 열렸으나 패킷이 오지 않음. 카운터 유지 + 재시도.
                return beginRetryOrFail(DiagnosisReason::NoPackets, ConnState::Reconnecting, now);
            }
            return {};
        case ConnState::Streaming:
            if (m_lastPacketAt && now - *m_lastPacketAt >= m_stall.stallTimeout) {
                return beginRetryOrFail(DiagnosisReason::NoPackets, ConnState::Stalled, now);
            }
            return {};
        case ConnState::Reconnecting:
        case ConnState::Stalled:
        case ConnState::Failed:   // 저빈도 재시도 (retryAt이 slowRetryDelay로 잡혀 있음)
            if (m_retryAt && now >= *m_retryAt) {
                m_state = ConnState::Connecting;
                m_retryAt.reset();
                return {Action::OpenSource};
            }
            return {};
        default:
            return {};  // Idle, Connecting: tick으로 변하지 않음
    }
}

} // namespace nv::domain
