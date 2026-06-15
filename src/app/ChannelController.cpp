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
                                     nv::domain::StallPolicy stall,
                                     bool useRelay)
    : m_channelId(std::move(channelId)), m_url(std::move(url)),
      m_useRelay(useRelay),
      m_source(source), m_clock(clock), m_logger(logger), m_sm(reconnect, stall) {
    // relay 모드: RelayIntake는 Unknown(헬스 주입 대기). 직결 모드: NotApplicable.
    if (!m_useRelay) {
        m_health.markNotApplicable(HealthStage::RelayIntake);
    }
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
    s.bitrateKbps = m_bitrateKbps;
    s.droppedFrames = m_droppedFrames;
    s.decodedFrames = m_decodedFrames;
    s.displayedFrames = m_displayedFrames;
    s.readBytesTotal = m_readBytesTotal;
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
    m_bytesInWindow = 0; m_bitrateKbps = 0.0; m_droppedFrames = 0;   // 새 연결 — 지표 리셋
    m_decodedFrames = 0; m_displayedFrames = 0; m_readBytesTotal = 0;
    apply(m_sm.connectRequested(m_clock.now()));
    logTransition("connect");
    notifyIfChanged();
}

void ChannelController::disconnect() {
    m_lastRateAt = m_clock.now(); m_packetsInWindow = 0; m_lastPacketAt.reset(); m_packetsPerSec = 0.0;
    m_bytesInWindow = 0; m_bitrateKbps = 0.0;   // 드롭 누적은 마지막 값 유지(연결 시 리셋)
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
        // bitrate(kbps) = 바이트*8/1000 / 초
        m_bitrateKbps = static_cast<double>(m_bytesInWindow) * 8.0 / static_cast<double>(elapsedMs);
        m_packetsInWindow = 0;
        m_bytesInWindow = 0;
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

void ChannelController::onBytesReceived(long long bytes) {
    if (!m_sourceAlive) return;
    m_bytesInWindow += bytes;   // tick에서 1초 윈도우로 bitrate 산출
    m_readBytesTotal += bytes;  // 누적(채널정보 Demux 데이터 크기)
}

void ChannelController::onFrameDropped() {
    if (!m_sourceAlive) return;
    ++m_droppedFrames;          // 누적 드롭(디코드/HW전송 실패)
}

void ChannelController::onFrameDecoded() {
    if (!m_sourceAlive) return;
    ++m_decodedFrames;                             // 누적(채널정보 비디오 디코드)
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
    ++m_displayedFrames;                           // 누적(채널정보 비디오 출력)
    apply(m_sm.framePresented(m_clock.now()));
    m_health.markReached(HealthStage::Presenting);
    if (!m_loggedPresented) {
        m_loggedPresented = true;
        m_logger.log(LogLevel::Info, m_channelId, "ChannelController",
                     "first frame presented", m_sm.lastReason());
    }
    notifyIfChanged();
}

void ChannelController::onRelayHealth(bool deviceLegUp, DiagnosisReason reasonIfDown) {
    if (!m_useRelay) return;
    if (deviceLegUp) {
        m_health.markReached(HealthStage::RelayIntake);
        notifySourceAvailable();  // Failed 상태이면 sourceAvailableHint로 즉시 재접속
    } else {
        m_health.markFailed(HealthStage::RelayIntake, reasonIfDown);
        notifyIfChanged();
    }
}

void ChannelController::onSourceError(DiagnosisReason reason) {
    if (!m_sourceAlive) return;
    // #1: relay 모드에서 연결 자체가 거부/도달불가면 장비가 아니라 로컬 relay가 죽은 것 → RelayDown.
    if (m_useRelay && reason == DiagnosisReason::DeviceUnreachable)
        reason = DiagnosisReason::RelayDown;
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
