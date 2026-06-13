#include "src/infra/relay/RelayServiceManagerFactory.h"

#if defined(_WIN32)
#  include "src/infra/relay/WindowsRelayService.h"
#elif defined(__APPLE__)
#  include "src/infra/relay/LaunchdRelayService.h"
#endif

namespace nv::infra {

std::unique_ptr<nv::app::IRelayServiceManager> makeRelayServiceManager() {
#if defined(_WIN32)
    // Windows: mediamtx.exe 경로는 인스톨러(M5)가 레지스트리/환경변수로 제공.
    // 여기서는 PATH에서 찾는다 (실제 배포에서는 절대 경로를 주입할 것).
    return std::make_unique<WindowsRelayService>("mediamtx.exe");
#elif defined(__APPLE__)
    return std::make_unique<LaunchdRelayService>();
#else
    // 미지원 플랫폼 — 호출자가 nullptr 처리
    return nullptr;
#endif
}

} // namespace nv::infra
