#pragma once
#include <string>
#include "IFrameSurface.h"

namespace nv::app {

class IFrameSurfaceRegistry {
public:
    virtual ~IFrameSurfaceRegistry() = default;
    // lastSeq보다 새 프레임이 있으면 out에 채우고 true. CPU/GPU 변종 모두 지원.
    virtual bool latestSurface(const std::string& channelId, FrameSurface& out,
                               uint64_t lastSeq) = 0;
};

} // namespace nv::app
