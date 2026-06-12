#pragma once
#include <functional>
#include <optional>
#include <string>
#include "src/domain/connection/ConnectionStateMachine.h"
#include "src/domain/health/StreamHealth.h"
#include "src/app/ports/IClock.h"
#include "src/app/ports/ILogger.h"
#include "src/app/ports/IStreamSource.h"
#include "ChannelSnapshot.h"

namespace nv::app {

// 채널 하나의 오케스트레이터: 상태머신의 Action을 포트 호출로 실행하고
// 진단 6단계(StreamHealth)를 유지한다.
// 전용 control 스레드에서만 사용 (설계 D6, 3차 리뷰 — UI 스레드 아님. 단일 스레드 보장이 핵심).
class ChannelController final : public StreamSourceListener {
public:
    ChannelController(std::string channelId, std::string url,
                      IStreamSource& source, const IClock& clock, ILogger& logger,
                      nv::domain::ReconnectPolicy reconnect, nv::domain::StallPolicy stall);

    void connect();
    void disconnect();
    void retry();                  // Failed에서 수동 재시도 (카운터 리셋)
    void notifySourceAvailable();  // relay 헬스체크의 소스 복귀 신호 (M4에서 배선, D1)
    void tick();                   // 1초 주기로 호출

    void setUrl(std::string url);  // Idle에서만 적용. 그 외 상태에선 무시.
    void setObserver(std::function<void(const ChannelSnapshot&)> obs);

    nv::domain::ConnState state() const { return m_sm.state(); }
    const nv::domain::StreamHealth& health() const { return m_health; }
    int reconnectAttempts() const { return m_sm.reconnectAttempts(); }

    // StreamSourceListener (어댑터 → 도메인 이벤트)
    void onSessionOpened() override;
    void onPacketReceived() override;
    void onFrameDecoded() override;
    void onFramePresented() override;
    void onSourceError(nv::domain::DiagnosisReason reason) override;

private:
    void apply(const nv::domain::Transition& t);
    void logTransition(std::string_view trigger);
    void notifyIfChanged();

    std::string m_channelId;
    std::string m_url;
    IStreamSource& m_source;
    const IClock& m_clock;
    ILogger& m_logger;
    nv::domain::ConnectionStateMachine m_sm;
    nv::domain::StreamHealth m_health;
    bool m_sourceAlive = false;   // close 이후 늦은 이벤트 차단
    std::function<void(const ChannelSnapshot&)> m_observer;
    ChannelSnapshot m_lastSnapshot;

    int m_packetsInWindow = 0;
    std::optional<std::chrono::steady_clock::time_point> m_lastPacketAt;
    std::chrono::steady_clock::time_point m_lastRateAt{};
    double m_packetsPerSec = 0.0;
};

} // namespace nv::app
