#pragma once
#include "src/app/ports/IChannelRepository.h"

namespace nv::test {

class FakeChannelRepository final : public nv::app::IChannelRepository {
public:
    std::vector<nv::domain::ChannelConfig> load() override { ++loadCount; return stored; }
    void save(const std::vector<nv::domain::ChannelConfig>& channels) override {
        ++saveCount;
        stored = channels;
    }
    std::vector<nv::domain::ChannelConfig> stored;
    int loadCount = 0;
    int saveCount = 0;
};

} // namespace nv::test
