#include "src/ui/relay/RelayCoordinator.h"
#include <QTimer>
#include <utility>

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
    m_sup.ensureUp(m_channels, m_configPath);
    m_timer = new QTimer(this);
    m_timer->setInterval(m_intervalMs);
    connect(m_timer, &QTimer::timeout, this, &RelayCoordinator::poll);
    m_timer->start();
}

// 워커 스레드의 이벤트루프에서 실행. pollHealth의 QNetworkAccessManager+QEventLoop가
// 워커 스레드에서 돌아가므로 UI 스레드에 영향 없고 GUI 스레드에 nested loop도 없다.
void RelayCoordinator::poll() {
    auto h = m_sup.pollHealth(m_channels);
    if (m_onHealth)
        m_onHealth(std::move(h));
}

// #6: 런타임 채널변경 반영. ensureUp은 멱등 + config-aware(commit 3191dfb) — config를 재생성하고
// 실제 변경이 있을 때만 relay를 재기동한다. 워커 스레드에서 실행(큐 마샬링으로 호출).
void RelayCoordinator::updateChannels(std::vector<nv::domain::RelayPath> channels) {
    m_channels = std::move(channels);
    m_sup.ensureUp(m_channels, m_configPath);
}

// 스레드 종료 전 폴타이머 중지 — relaySup 파괴 후 poll()이 도는 use-after-free 방지.
void RelayCoordinator::shutdown() {
    if (m_timer)
        m_timer->stop();
}

} // namespace nv::ui
