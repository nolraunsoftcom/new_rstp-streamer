#pragma once
#include <vector>
#include "src/domain/channel/ChannelConfig.h"

namespace nv::app {

class IChannelRepository {
public:
    virtual ~IChannelRepository() = default;
    virtual std::vector<nv::domain::ChannelConfig> load() = 0;
    virtual bool save(const std::vector<nv::domain::ChannelConfig>& channels) = 0;
};

} // namespace nv::app
