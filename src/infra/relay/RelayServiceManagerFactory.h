#pragma once
#include "src/app/ports/IRelayServiceManager.h"
#include <memory>

namespace nv::infra {

// 플랫폼별 IRelayServiceManager 생성.
//   Windows  → WindowsRelayService (SCM + schtasks 폴백)
//   macOS    → LaunchdRelayService (LaunchAgent)
//   기타     → nullptr (미지원 플랫폼)
std::unique_ptr<nv::app::IRelayServiceManager> makeRelayServiceManager();

} // namespace nv::infra
