#include "ChannelSourceFactory.h"

namespace nv::infra {

std::unique_ptr<nv::app::IStreamSource> ChannelSourceFactory::createSource(
    const std::string& channelId) {
    LatestFrameSlot* slot = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto& s = m_slots[channelId];
        if (!s) s = std::make_unique<LatestFrameSlot>();
        slot = s.get();
    }
    return std::make_unique<Bundle>(*slot, m_executor);
}

void ChannelSourceFactory::destroySource(const std::string& channelId) {
    (void)channelId;   // 슬롯은 의도적으로 보관 (헤더 주석 참조)
}

LatestFrameSlot* ChannelSourceFactory::slot(const std::string& channelId) {
    std::lock_guard lk(m_mu);
    auto it = m_slots.find(channelId);
    return it == m_slots.end() ? nullptr : it->second.get();
}

} // namespace nv::infra
