// VtMetalBridge — 비-Apple 플랫폼용 no-op 스텁.
//
// 실구현은 VtMetalBridge.mm(Obj-C++, Apple 전용, CMake에서 APPLE에서만 추가).
// 헤더(VtMetalBridge.h)는 플랫폼 중립이라 RhiVideoRenderer가 멤버로 들고 호출하므로,
// 비-Apple에서도 심볼 정의가 필요하다(없으면 링크 에러). 전 메서드는 false/no-op이며
// init()이 false를 돌려주므로 호출측은 NV12 zero-copy 경로 대신 RGBA(CPU) 경로로 폴백한다.
#ifndef __APPLE__

#include "src/infra/video/VtMetalBridge.h"

namespace nv::infra {

VtMetalBridge::~VtMetalBridge() = default;

bool VtMetalBridge::init(void* /*mtlDevice*/) { return false; }

bool VtMetalBridge::map(void* /*cvPixelBuffer*/, PlaneTextures& /*out*/) { return false; }

void VtMetalBridge::unmap(PlaneTextures& /*planes*/) {}

void VtMetalBridge::flush() {}

} // namespace nv::infra

#endif // !__APPLE__
