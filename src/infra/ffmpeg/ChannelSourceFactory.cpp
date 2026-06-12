#include "ChannelSourceFactory.h"

namespace nv::infra {

std::unique_ptr<nv::app::IStreamSource> ChannelSourceFactory::createSource(
    const std::string& channelId) {
    LatestSurfaceSlot* slot = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto& s = m_slots[channelId];
        if (!s) s = std::make_unique<LatestSurfaceSlot>();
        slot = s.get();
    }
    return std::make_unique<Bundle>(*slot, m_executor);
}

void ChannelSourceFactory::destroySource(const std::string& channelId) {
    (void)channelId;   // 슬롯은 의도적으로 보관 (헤더 주석 참조)
}

bool ChannelSourceFactory::latestSurface(const std::string& channelId,
                                         nv::app::FrameSurface& out, uint64_t lastSeq) {
    LatestSurfaceSlot* s = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto it = m_slots.find(channelId);
        if (it == m_slots.end()) return false;
        s = it->second.get();
    }
    return s->latest(out, lastSeq);   // 슬롯 자체 뮤텍스로 보호
}

void ChannelSourceFactory::releaseConsumed(const std::string& channelId, void* handle) {
    LatestSurfaceSlot* s = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto it = m_slots.find(channelId);
        if (it == m_slots.end()) return;
        s = it->second.get();
    }
    s->releaseConsumed(handle);
}

LatestSurfaceSlot* ChannelSourceFactory::slot(const std::string& channelId) {
    std::lock_guard lk(m_mu);
    auto it = m_slots.find(channelId);
    return it == m_slots.end() ? nullptr : it->second.get();
}

std::vector<std::string> ChannelSourceFactory::slotIds() {
    std::lock_guard lk(m_mu);
    std::vector<std::string> ids;
    ids.reserve(m_slots.size());
    for (const auto& [id, _] : m_slots) ids.push_back(id);
    return ids;
}

} // namespace nv::infra
