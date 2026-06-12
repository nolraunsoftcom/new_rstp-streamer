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
            m_loggedDecoded   = false;
            m_loggedPresented = false;
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

void ChannelController::setUrl(std::string url) {
    if (m_sm.state() != ConnState::Idle) return;
    m_url = std::move(url);
}

void ChannelController::setObserver(std::function<void(const ChannelSnapshot&)> obs) {
    m_observer = std::move(obs);
}

void ChannelController::notifyIfChanged() {
    if (!m_observer) return;
    ChannelSnapshot s{m_sm.state(), m_sm.reconnectAttempts(), m_sm.lastReason(), m_health};
    s.packetsPerSec = m_packetsPerSec;
    s.msSinceLastPacket =
        m_lastPacketAt ? std::chrono::duration_cast<std::chrono::milliseconds>(
                             m_clock.now() - *m_lastPacketAt)
                             .count()
                       : -1;
    if (s == m_lastSnapshot) return;
    m_lastSnapshot = s;
    m_observer(s);
}

void ChannelController::connect() {
    m_lastRateAt = m_clock.now(); m_packetsInWindow = 0; m_lastPacketAt.reset(); m_packetsPerSec = 0.0;
    apply(m_sm.connectRequested(m_clock.now()));
    logTransition("connect");
    notifyIfChanged();
}

void ChannelController::disconnect() {
    m_lastRateAt = m_clock.now(); m_packetsInWindow = 0; m_lastPacketAt.reset(); m_packetsPerSec = 0.0;
    apply(m_sm.disconnectRequested(m_clock.now()));
    logTransition("disconnect");
    notifyIfChanged();
}

void ChannelController::retry() {
    apply(m_sm.retryRequested(m_clock.now()));
    logTransition("retry");
    notifyIfChanged();
}

void ChannelController::notifySourceAvailable() {
    const auto before = m_sm.state();
    apply(m_sm.sourceAvailableHint(m_clock.now()));
    if (m_sm.state() != before) logTransition("sourceAvailable");
    notifyIfChanged();
}

void ChannelController::tick() {
    const auto now = m_clock.now();
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastRateAt).count();
    if (elapsedMs > 0) {
        m_packetsPerSec = m_packetsInWindow * 1000.0 / static_cast<double>(elapsedMs);
        m_packetsInWindow = 0;
        m_lastRateAt = now;
    }

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
    // markFailed 직후 apply(OpenSource→health.reset())로 실패 기록이 곧바로 지워질 수 있다.
    // 의도된 동작: 실패는 로그에 남고, health는 새 사이클 상태를 보여준다.
    apply(t);
    notifyIfChanged();
}

void ChannelController::onSessionOpened() {
    if (!m_sourceAlive) return;
    apply(m_sm.sessionOpened(m_clock.now()));
    if (m_sm.state() == ConnState::SessionOpen) {
        // 세션이 열렸다는 것은 장비(또는 relay)까지 도달했다는 뜻
        m_health.markReached(HealthStage::RtspSession);
        logTransition("sessionOpened");
    }
    notifyIfChanged();
}

void ChannelController::onPacketReceived() {
    if (!m_sourceAlive) return;
    ++m_packetsInWindow; m_lastPacketAt = m_clock.now();
    apply(m_sm.packetReceived(m_clock.now()));
    m_health.markReached(HealthStage::PacketFlow);
    notifyIfChanged();
}

void ChannelController::onFrameDecoded() {
    if (!m_sourceAlive) return;
    m_health.markReached(HealthStage::Decoding);   // 상태머신 전이 없음 — 진단 전용
    if (!m_loggedDecoded) {
        m_loggedDecoded = true;
        m_logger.log(LogLevel::Info, m_channelId, "ChannelController",
                     "first frame decoded", m_sm.lastReason());
    }
    notifyIfChanged();
}

void ChannelController::onFramePresented() {
    if (!m_sourceAlive) return;
    apply(m_sm.framePresented(m_clock.now()));
    m_health.markReached(HealthStage::Presenting);
    if (!m_loggedPresented) {
        m_loggedPresented = true;
        m_logger.log(LogLevel::Info, m_channelId, "ChannelController",
                     "first frame presented", m_sm.lastReason());
    }
    notifyIfChanged();
}

void ChannelController::onSourceError(DiagnosisReason reason) {
    if (!m_sourceAlive) return;
    const auto stage = (m_sm.state() == ConnState::Streaming) ? HealthStage::PacketFlow
                                                              : HealthStage::RtspSession;
    apply(m_sm.errorOccurred(reason, m_clock.now()));
    // 의도된 분기: health에는 근접 원인(reason)을 기록한다. give-up 경계에서 SM의
    // lastReason은 GaveUp(정책 결정)이 되지만, 운영자 진단에는 근접 원인이 더 유용하다.
    m_health.markFailed(stage, reason);
    logTransition("sourceError");
    notifyIfChanged();
}

} // namespace nv::app
