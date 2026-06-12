#include "ChannelManager.h"
#include <algorithm>
#include <cstdlib>

namespace nv::app {

using nv::domain::ChannelConfig;

ChannelManager::ChannelManager(IChannelRepository& repo, IChannelRuntimeFactory& factory,
                               const IClock& clock, ILogger& logger,
                               nv::domain::ReconnectPolicy reconnect,
                               nv::domain::StallPolicy stall, int maxChannels)
    : m_repo(repo), m_factory(factory), m_clock(clock), m_logger(logger),
      m_reconnect(reconnect), m_stall(stall), m_maxChannels(maxChannels) {}

// Fix 6: makeEntry와 setSnapshotObserver rebind 루프의 동일 람다를 헬퍼로 추출
void ChannelManager::bindObserver(const std::string& id, Entry& e) {
    auto cb = m_snapshotObserver;
    const std::string entryId = id;
    e.ctrl->setObserver([cb, entryId](const ChannelSnapshot& s) { cb(entryId, s); });
}

ChannelManager::Entry& ChannelManager::makeEntry(ChannelConfig cfg) {
    const std::string id = cfg.id;
    auto source = m_factory.createSource(id);
    auto ctrl = std::make_unique<ChannelController>(id, cfg.url, *source, m_clock, m_logger,
                                                    m_reconnect, m_stall);
    auto [it, ok] = m_entries.emplace(id,
                                      Entry{std::move(cfg), std::move(source), std::move(ctrl)});
    (void)ok;
    // 스냅샷 옵저버가 이미 설정돼 있으면 즉시 바인딩
    if (m_snapshotObserver) bindObserver(id, it->second);
    return it->second;
}

void ChannelManager::restore(bool autoConnect) {
    for (auto& cfg : m_repo.load()) {
        if (channelCount() >= m_maxChannels) break;
        makeEntry(cfg);
    }
    if (autoConnect) connectAll();
    notifyList();
}

int ChannelManager::nextGridIndex() const {
    int idx = 0;
    for (;;) {
        bool taken = false;
        for (auto& [_, e] : m_entries)
            if (e.cfg.gridIndex == idx) { taken = true; break; }
        if (!taken) return idx;
        ++idx;
    }
}

int ChannelManager::nextIdNumber() const {
    int maxN = 0;
    for (auto& [id, _] : m_entries) {
        if (id.rfind("ch", 0) == 0)
            maxN = std::max(maxN, std::atoi(id.c_str() + 2));
    }
    return maxN + 1;
}

std::string ChannelManager::addChannel(std::string name, std::string url) {
    if (channelCount() >= m_maxChannels) return {};
    ChannelConfig cfg;
    cfg.id = "ch" + std::to_string(nextIdNumber());
    cfg.name = std::move(name);
    cfg.url = std::move(url);
    cfg.gridIndex = nextGridIndex();
    // map 삽입 후 std::prev(end()) 는 키 정렬 순서라 id와 다를 수 있으므로
    // id를 미리 지역 변수로 보관한다.
    const std::string id = cfg.id;
    makeEntry(std::move(cfg));
    persist();
    notifyList();
    return id;
}

void ChannelManager::removeChannel(const std::string& id) {
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return;
    it->second.ctrl->disconnect();          // 소스 정리 (TEARDOWN 경유)
    m_entries.erase(it);                    // ctrl → source 순 파괴
    m_factory.destroySource(id);
    persist();
    notifyList();
}

void ChannelManager::updateChannel(const std::string& id, std::string name, std::string url) {
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return;
    auto& e = it->second;
    e.cfg.name = std::move(name);
    const bool urlChanged = (e.cfg.url != url);
    e.cfg.url = url;
    if (urlChanged) {
        e.ctrl->disconnect();               // Failed 함정 방지 — main.cpp 연결 버튼과 동일 시퀀스
        e.ctrl->setUrl(std::move(url));
        e.ctrl->connect();
    }
    persist();
    notifyList();
}

void ChannelManager::swapGrid(const std::string& idA, const std::string& idB) {
    auto a = m_entries.find(idA);
    auto b = m_entries.find(idB);
    if (a == m_entries.end() || b == m_entries.end()) return;
    std::swap(a->second.cfg.gridIndex, b->second.cfg.gridIndex);
    persist();
    notifyList();
}

void ChannelManager::connectAll() {
    for (auto& [_, e] : m_entries) e.ctrl->connect();
}

void ChannelManager::disconnectAll() {
    for (auto& [_, e] : m_entries) e.ctrl->disconnect();
}

void ChannelManager::tickAll() {
    for (auto& [_, e] : m_entries) e.ctrl->tick();
}

std::vector<ChannelConfig> ChannelManager::configs() const {
    std::vector<ChannelConfig> out;
    out.reserve(m_entries.size());
    for (auto& [_, e] : m_entries) out.push_back(e.cfg);
    std::sort(out.begin(), out.end(),
              [](const auto& x, const auto& y) { return x.gridIndex < y.gridIndex; });
    return out;
}

const ChannelConfig& ChannelManager::config(const std::string& id) const {
    return m_entries.at(id).cfg;
}

ChannelController* ChannelManager::controller(const std::string& id) {
    auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : it->second.ctrl.get();
}

void ChannelManager::setSnapshotObserver(
    std::function<void(const std::string&, const ChannelSnapshot&)> obs) {
    m_snapshotObserver = std::move(obs);
    // 옵저버가 나중에 설정된 경우 기존 엔트리에도 재바인딩 (Fix 6: bindObserver 헬퍼 사용)
    if (m_snapshotObserver) {
        for (auto& [id, e] : m_entries) bindObserver(id, e);
    } else {
        for (auto& [id, e] : m_entries) e.ctrl->setObserver(nullptr);
    }
}

void ChannelManager::persist() {
    m_repo.save(configs());
}

void ChannelManager::notifyList() {
    if (m_listChanged) m_listChanged();
}

} // namespace nv::app
