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
//
// **스레드 계약: 모든 메서드(옵저버 설정 포함)는 control 스레드(executor)에서만 호출한다.**
// UI/조립부는 executor.post로 진입할 것.
class ChannelManager {
public:
    static constexpr int kPerformanceTargetChannels = 20;  // 성능 게이트 기준선 (M2b)
    static constexpr int kDefaultMaxChannels = 32;         // 소프트 한도 (주입으로 조정 가능)

    ChannelManager(IChannelRepository& repo, IChannelRuntimeFactory& factory,
                   const IClock& clock, ILogger& logger,
                   nv::domain::ReconnectPolicy reconnect, nv::domain::StallPolicy stall,
                   int maxChannels = kDefaultMaxChannels);

    void restore(bool autoConnect);                        // 저장본 로드 (시작 시 1회)
    std::string addChannel(std::string name, std::string url, bool autoConnect = false,
                           bool useRelay = false);   // 실패(한도) 시 "" 반환
    void removeChannel(const std::string& id);
    void updateChannel(const std::string& id, std::string name, std::string url,
                       bool autoConnect = false, bool useRelay = false);
    void swapGrid(const std::string& idA, const std::string& idB);   // 그리드 위치 교환(gridIndex만)
    // 그리드 위치 이동(gridIndex만). 대상 셀이 점유면 두 채널 위치 교환, 비면 그대로 이동.
    // 레거시 moveViewerToGridIndex 대응. persist + notifyList.
    void moveGrid(const std::string& id, int targetGridIndex);
    // 채널 리스트 순서 재배열(listIndex만, 그리드와 독립). `order`는 새 표시 순서의 id 목록.
    // 목록에 없는 채널은 끝에 안정적으로 추가된다. persist + notifyList.
    void reorderList(const std::vector<std::string>& order);

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
    void bindObserver(const std::string& id, Entry& e);  // Fix 6: 옵저버 바인딩 중복 제거
    void persist();
    void notifyList();
    int nextGridIndex() const;
    int nextListIndex() const;
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
