#include "src/ui/relay/RelayCoordinator.h"
#include <QTimer>
#include <algorithm>
#include <utility>
#include "src/domain/health/DiagnosisReason.h"

namespace nv::ui {

RelayCoordinator::RelayCoordinator(nv::app::RelaySupervisor& sup,
                                   std::vector<nv::domain::RelayPath> channels,
                                   std::string configPath,
                                   int pollIntervalMs,
                                   HealthCallback onHealth,
                                   QObject* parent)
    : QObject(parent),
      m_sup(sup),
      m_channels(std::move(channels)),
      m_configPath(std::move(configPath)),
      m_intervalMs(pollIntervalMs),
      m_onHealth(std::move(onHealth)) {}

// 워커 스레드에서 호출(QThread::started 시그널로 트리거). ensureUp의 launchctl 블로킹(~10s)이
// 이제 UI 스레드가 아니라 워커 스레드에서 일어난다. 폴타이머는 여기서 생성해야 워커 스레드에
// affinity를 가진다(timeout→poll이 워커 이벤트루프에서 실행됨).
void RelayCoordinator::start() {
    // relay 채널이 하나도 없으면 mediamtx를 띄우지 않는다(불필요한 기동·macOS 로컬네트워크
    // 권한 프롬프트 방지). 런타임에 첫 relay 채널이 생기면 updateChannels에서 ensureUp 한다.
    if (!m_channels.empty())
        m_sup.ensureUp(m_channels, m_configPath);
    m_timer = new QTimer(this);
    m_timer->setInterval(m_intervalMs);
    connect(m_timer, &QTimer::timeout, this, &RelayCoordinator::poll);
    m_timer->start();
}

// 워커 스레드의 이벤트루프에서 실행. pollHealth의 QNetworkAccessManager+QEventLoop가
// 워커 스레드에서 돌아가므로 UI 스레드에 영향 없고 GUI 스레드에 nested loop도 없다.
void RelayCoordinator::poll() {
    // relay 채널이 없으면 헬스 폴링 자체를 건너뛴다 — pollHealth는 채널 유무와 무관하게
    // Control API를 호출하므로, mediamtx 미기동 시 불필요한 "Connection refused" 로그가 난다.
    if (m_channels.empty()) return;
    auto h = m_sup.pollHealth(m_channels);

    // 셀프힐: 모든 relay 채널이 RelayDown(서비스 무응답)으로 연속 관측되면 mediamtx를 강제
    // 재기동한다. launchd KeepAlive는 크래시는 살리지만 wedge(살아있으나 무응답)는 못 살린다.
    if (m_cooldown > 0) {
        --m_cooldown;   // 재기동 직후 기동대기(~10s) 동안은 오탐 방지로 카운트하지 않음
    } else {
        const bool allDown = !h.empty() &&
            std::all_of(h.begin(), h.end(), [](const auto& kv) {
                return kv.second.reason == nv::domain::DiagnosisReason::RelayDown;
            });
        if (allDown) {
            if (++m_unhealthyStreak >= kUnhealthyThreshold) {
                m_sup.restart(m_channels, m_configPath);
                m_unhealthyStreak = 0;
                m_cooldown = kCooldownPolls;
            }
        } else {
            m_unhealthyStreak = 0;
        }
    }

    if (m_onHealth)
        m_onHealth(std::move(h));
}

// #6: 런타임 채널변경 반영. ensureUp은 멱등 + config-aware(commit 3191dfb) — config를 재생성하고
// 실제 변경이 있을 때만 relay를 재기동한다. 워커 스레드에서 실행(큐 마샬링으로 호출).
void RelayCoordinator::updateChannels(std::vector<nv::domain::RelayPath> channels) {
    m_channels = std::move(channels);
    // 첫 relay 채널이 생긴 시점(또는 채널 변경)에만 ensureUp. 빈 목록이면 mediamtx를 새로
    // 띄우지 않는다(이미 떠 있던 경우 stop API가 없어 앱 종료 시 teardown까지 유지된다).
    if (!m_channels.empty())
        m_sup.ensureUp(m_channels, m_configPath);
}

// 스레드 종료 전 폴타이머 중지 — relaySup 파괴 후 poll()이 도는 use-after-free 방지.
void RelayCoordinator::shutdown() {
    if (m_timer)
        m_timer->stop();
}

} // namespace nv::ui
