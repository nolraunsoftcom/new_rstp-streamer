#include "ChannelManager.h"
#include <algorithm>
#include <cstdlib>

namespace nv::app {

// S2: rtsp:// 또는 rtsps:// 스킴만 허용 (F6: RFC 스킴 대소문자 무관 — 소문자 변환 후 비교)
static bool isValidRtspUrl(const std::string& url) {
    // 스킴 최대 길이 8 ("rtsps://") 만큼만 소문자로 변환해 비교
    const size_t prefixLen = std::min(url.size(), size_t(8));
    std::string lower(url.begin(), url.begin() + static_cast<std::ptrdiff_t>(prefixLen));
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.rfind("rtsp://", 0) == 0 || lower.rfind("rtsps://", 0) == 0;
}

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
    // relay 모드: 앱이 연결할 URL은 로컬 relay(127.0.0.1:8554/<id>), cfg.url은 장비 URL 유지.
    // 직결 모드: cfg.url 그대로.
    const std::string connectUrl = cfg.useRelay
                                       ? nv::domain::relayUrlFor(id)
                                       : cfg.url;
    auto source = m_factory.createSource(id);
    auto ctrl = std::make_unique<ChannelController>(id, connectUrl, *source, m_clock, m_logger,
                                                    m_reconnect, m_stall, cfg.useRelay);
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
        // F4: restore 경로도 URL 검증 — 부적합 항목은 스킵 + 경고 로그
        if (!isValidRtspUrl(cfg.url)) {
            m_logger.log(LogLevel::Warn, cfg.id, "ChannelManager",
                         "invalid url skipped on restore",
                         nv::domain::DiagnosisReason::DeviceUnreachable);
            continue;
        }
        makeEntry(cfg);
    }
    // 전역 --connect 또는 채널별 자동 연결 플래그
    for (auto& [_, e] : m_entries)
        if (autoConnect || e.cfg.autoConnect) e.ctrl->connect();
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

int ChannelManager::nextListIndex() const {
    int idx = 0;
    for (;;) {
        bool taken = false;
        for (auto& [_, e] : m_entries)
            if (e.cfg.listIndex == idx) { taken = true; break; }
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

std::string ChannelManager::addChannel(std::string name, std::string url, bool autoConnect) {
    if (!isValidRtspUrl(url)) return {};   // S2: 비 RTSP 스킴 거부
    if (channelCount() >= m_maxChannels) return {};
    ChannelConfig cfg;
    cfg.id = "ch" + std::to_string(nextIdNumber());
    cfg.name = std::move(name);
    cfg.url = std::move(url);
    cfg.gridIndex = nextGridIndex();
    cfg.listIndex = nextListIndex();
    cfg.autoConnect = autoConnect;
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

void ChannelManager::updateChannel(const std::string& id, std::string name, std::string url,
                                   bool autoConnect) {
    if (!isValidRtspUrl(url)) return;       // S2: 비 RTSP 스킴 거부
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return;
    auto& e = it->second;
    e.cfg.name = std::move(name);
    const bool urlChanged = (e.cfg.url != url);
    e.cfg.url = url;
    e.cfg.autoConnect = autoConnect;
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

void ChannelManager::moveGrid(const std::string& id, int targetGridIndex) {
    auto it = m_entries.find(id);
    if (it == m_entries.end() || targetGridIndex < 0) return;
    const int sourceGridIndex = it->second.cfg.gridIndex;
    if (sourceGridIndex == targetGridIndex) return;

    // 대상 셀을 점유한 다른 채널이 있으면 그 채널을 출발 셀로 보낸다(=교환). 비어있으면 이동만.
    for (auto& [otherId, e] : m_entries) {
        if (otherId != id && e.cfg.gridIndex == targetGridIndex) {
            e.cfg.gridIndex = sourceGridIndex;
            break;
        }
    }
    it->second.cfg.gridIndex = targetGridIndex;
    persist();
    notifyList();
}

void ChannelManager::reorderList(const std::vector<std::string>& order) {
    int idx = 0;
    // 1) 전달된 순서대로 listIndex 재부여(존재하는 채널만).
    for (const auto& id : order) {
        auto it = m_entries.find(id);
        if (it != m_entries.end()) it->second.cfg.listIndex = idx++;
    }
    // 2) order에 없던 채널은 기존 listIndex 순서를 보존해 뒤에 안정적으로 이어붙인다.
    std::vector<const Entry*> rest;
    for (auto& [id, e] : m_entries)
        if (std::find(order.begin(), order.end(), id) == order.end())
            rest.push_back(&e);
    std::sort(rest.begin(), rest.end(),
              [](const Entry* x, const Entry* y) { return x->cfg.listIndex < y->cfg.listIndex; });
    for (const Entry* e : rest)
        m_entries.at(e->cfg.id).cfg.listIndex = idx++;
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
    if (!m_repo.save(configs())) {  // U3: 저장 실패 시 경고 로그
        m_logger.log(LogLevel::Warn, "", "ChannelManager", "channel save failed",
                     nv::domain::DiagnosisReason::DiskFull);
    }
}

void ChannelManager::notifyList() {
    if (m_listChanged) m_listChanged();
}

} // namespace nv::app
