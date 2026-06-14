#pragma once
#include <QObject>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "src/domain/relay/RelayConfig.h"        // RelayPath
#include "src/app/RelaySupervisor.h"             // RelaySupervisor, RelayChannelHealth
class QTimer;
namespace nv::ui {
// relay 수명(ensureUp + 헬스폴링)을 전용 워커 스레드에서 돌린다. main/UI 스레드 블로킹 제거.
// 헬스 결과는 onHealth 콜백으로 넘기며, 콜백 내부에서 control 스레드(executor)로 마샬한다.
class RelayCoordinator : public QObject {
    Q_OBJECT
public:
    using HealthCallback = std::function<void(std::map<std::string, nv::app::RelayChannelHealth>)>;
    RelayCoordinator(nv::app::RelaySupervisor& sup,
                     std::vector<nv::domain::RelayPath> channels,
                     std::string configPath,
                     int pollIntervalMs,
                     HealthCallback onHealth,
                     QObject* parent = nullptr);
public slots:
    void start();                                              // 워커 스레드에서: ensureUp 1회 + 폴타이머 시작
    void updateChannels(std::vector<nv::domain::RelayPath> channels);  // #6: 채널변경 → 재생성+ensureUp(멱등, 설정변경시만 재기동)
    void shutdown();                                           // 폴타이머 중지(스레드 종료 전)
private slots:
    void poll();
private:
    nv::app::RelaySupervisor& m_sup;
    std::vector<nv::domain::RelayPath> m_channels;
    std::string m_configPath;
    int m_intervalMs;
    HealthCallback m_onHealth;
    QTimer* m_timer = nullptr;
};
}
