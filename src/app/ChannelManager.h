#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "ChannelController.h"
#include "ChannelSnapshot.h"
#include "ports/IChannelRepository.h"
#include "ports/IChannelRuntimeFactory.h"

namespace nv::app {

// 채널 N개의 수명·배치·영속을 관리한다. 모든 메서드는 control 스레드에서만 호출 (설계 D6).
// 채널 독립(설계 R2): 한 채널의 추가/삭제/장애가 다른 채널의 소스·컨트롤러를 건드리지 않는다.
class ChannelManager {
public:
    static constexpr int kPerformanceTargetChannels = 20;  // 성능 게이트 기준선 (M2b)
    static constexpr int kDefaultMaxChannels = 32;         // 소프트 한도 (주입으로 조정 가능)

    ChannelManager(IChannelRepository& repo, IChannelRuntimeFactory& factory,
                   const IClock& clock, ILogger& logger,
                   nv::domain::ReconnectPolicy reconnect, nv::domain::StallPolicy stall,
                   int maxChannels = kDefaultMaxChannels);

    void restore(bool autoConnect);                        // 저장본 로드 (시작 시 1회)
    std::string addChannel(std::string name, std::string url);   // 실패(한도) 시 "" 반환
    void removeChannel(const std::string& id);
    void updateChannel(const std::string& id, std::string name, std::string url);
    void swapGrid(const std::string& idA, const std::string& idB);

    void connectAll();
    void disconnectAll();
    void tickAll();

    int channelCount() const { return static_cast<int>(m_entries.size()); }
    std::vector<nv::domain::ChannelConfig> configs() const;       // gridIndex 순 정렬
    const nv::domain::ChannelConfig& config(const std::string& id) const;
    ChannelController* controller(const std::string& id);

    void setListChangedObserver(std::function<void()> obs) { m_listChanged = std::move(obs); }
    void setSnapshotObserver(std::function<void(const std::string&, const ChannelSnapshot&)> obs);

private:
    struct Entry {
        nv::domain::ChannelConfig cfg;
        std::unique_ptr<IStreamSource> source;     // factory 산출물 (먼저 선언 — ctrl보다 오래 산다)
        std::unique_ptr<ChannelController> ctrl;
    };

    Entry& makeEntry(nv::domain::ChannelConfig cfg);
    void persist();
    void notifyList();
    int nextGridIndex() const;
    int nextIdNumber() const;

    IChannelRepository& m_repo;
    IChannelRuntimeFactory& m_factory;
    const IClock& m_clock;
    ILogger& m_logger;
    nv::domain::ReconnectPolicy m_reconnect;
    nv::domain::StallPolicy m_stall;
    std::map<std::string, Entry> m_entries;        // id → entry
    std::function<void()> m_listChanged;
    std::function<void(const std::string&, const ChannelSnapshot&)> m_snapshotObserver;
    int m_maxChannels;
};

} // namespace nv::app
