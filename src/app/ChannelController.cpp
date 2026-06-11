#include "ChannelController.h"

namespace nv::app {

using nv::domain::Action;
using nv::domain::ConnState;
using nv::domain::DiagnosisReason;
using nv::domain::HealthStage;
using nv::domain::Transition;

ChannelController::ChannelController(std::string channelId, std::string url,
                                     IStreamSource& source, const IClock& clock, ILogger& logger,
                                     nv::domain::ReconnectPolicy reconnect,
                                     nv::domain::StallPolicy stall)
    : m_channelId(std::move(channelId)), m_url(std::move(url)),
      m_source(source), m_clock(clock), m_logger(logger), m_sm(reconnect, stall) {
    // M1은 직결 모드 — relay 단계는 측정 대상이 아님 (M4에서 모드별 설정으로 바뀐다)
    m_health.markNotApplicable(HealthStage::RelayIntake);
}

void ChannelController::apply(const Transition& t) {
    switch (t.action) {
        case Action::OpenSource:
            m_health.reset();
            m_sourceAlive = true;
            m_source.open(m_url, *this);
            break;
        case Action::CloseSource:
            m_sourceAlive = false;
            m_source.close();
            break;
        case Action::None:
            break;
    }
}

void ChannelController::logTransition(std::string_view trigger) {
    m_logger.log(LogLevel::Info, m_channelId, "ChannelController",
                 std::string(trigger) + " -> " + std::string(toString(m_sm.state())),
                 m_sm.lastReason());
}

void ChannelController::connect() {
    apply(m_sm.connectRequested(m_clock.now()));
    logTransition("connect");
}

void ChannelController::disconnect() {
    apply(m_sm.disconnectRequested(m_clock.now()));
    logTransition("disconnect");
}

void ChannelController::retry() {
    apply(m_sm.retryRequested(m_clock.now()));
    logTransition("retry");
}

void ChannelController::notifySourceAvailable() {
    const auto before = m_sm.state();
    apply(m_sm.sourceAvailableHint(m_clock.now()));
    if (m_sm.state() != before) logTransition("sourceAvailable");
}

void ChannelController::tick() {
    const auto before = m_sm.state();
    auto t = m_sm.tick(m_clock.now());
    if (m_sm.state() != before) {
        // 시간 기반 실패(가짜연결/stall)는 패킷 단계 실패로 기록
        if (m_sm.state() == ConnState::Reconnecting || m_sm.state() == ConnState::Stalled ||
            m_sm.state() == ConnState::Failed) {
            m_health.markFailed(HealthStage::PacketFlow, m_sm.lastReason());
        }
        logTransition("tick");
    }
    apply(t);
}

void ChannelController::onSessionOpened() {
    if (!m_sourceAlive) return;
    apply(m_sm.sessionOpened(m_clock.now()));
    if (m_sm.state() == ConnState::SessionOpen) {
        // 세션이 열렸다는 것은 장비(또는 relay)까지 도달했다는 뜻
        m_health.markReached(HealthStage::RtspSession);
        logTransition("sessionOpened");
    }
}

void ChannelController::onPacketReceived() {
    if (!m_sourceAlive) return;
    apply(m_sm.packetReceived(m_clock.now()));
    m_health.markReached(HealthStage::PacketFlow);
}

void ChannelController::onFrameDecoded() {
    if (!m_sourceAlive) return;
    m_health.markReached(HealthStage::Decoding);   // 상태머신 전이 없음 — 진단 전용
}

void ChannelController::onFramePresented() {
    if (!m_sourceAlive) return;
    apply(m_sm.framePresented(m_clock.now()));
    m_health.markReached(HealthStage::Presenting);
}

void ChannelController::onSourceError(DiagnosisReason reason) {
    if (!m_sourceAlive) return;
    const auto stage = (m_sm.state() == ConnState::Streaming) ? HealthStage::PacketFlow
                                                              : HealthStage::RtspSession;
    apply(m_sm.errorOccurred(reason, m_clock.now()));
    m_health.markFailed(stage, reason);
    logTransition("sourceError");
}

} // namespace nv::app
