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
    // GpuTexture 서피스를 그린 뒤 핸들 ref를 반납. CpuRgba면 호출 불필요.
    virtual void releaseConsumed(const std::string& channelId, void* handle) = 0;
};

} // namespace nv::app
