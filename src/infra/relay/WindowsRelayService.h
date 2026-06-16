#pragma once
#ifdef _WIN32

#include "src/app/ports/IRelayServiceManager.h"
#include <string>

namespace nv::infra {

// Windows SCM(서비스) 기반 IRelayServiceManager 구현 — 배포 1순위.
//
// ensureRunning 전략:
//   1) sc query NewViewerRelay  — 이미 설치됐으면 sc start(멱등).
//   2) 미설치 + 관리자 권한: sc create + sc start.
//      (M5 NSIS 인스톨러가 관리자 권한으로 1회 등록 → 런타임은 sc start만 필요)
//   3) 미설치 + 관리자 없음(폴백): schtasks /create(OnLogon) + schtasks /run.
//      설치 없이 사용자 세션에서 자동기동. status.detail에 "fallback:schtasks" 기록.
//
// 주의: 런타임 검증은 Windows 박스 + 장비 환경에서 별도 수행(이 파일은 mac 빌드 제외).
class WindowsRelayService : public nv::app::IRelayServiceManager {
public:
    // mediamtxExe: mediamtx.exe 절대 경로 (인스톨러가 전달).
    explicit WindowsRelayService(std::string mediamtxExe,
                                 std::string serviceName = "NewViewerRelay");

    bool ensureRunning(const std::string& configPath) override;
    nv::app::RelayServiceStatus status() const override;
    bool stop() override;

private:
    std::string m_exe;       // mediamtx.exe 절대 경로(UTF-8)
    std::string m_svcName;   // (구 SCM 서비스명 — 현재 미사용, 인터페이스 호환 유지)
};

} // namespace nv::infra

#endif // _WIN32
