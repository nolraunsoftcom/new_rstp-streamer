#pragma once
#ifdef __APPLE__

#include "src/app/ports/IRelayServiceManager.h"
#include <string>

namespace nv::infra {

// macOS LaunchAgent 기반 IRelayServiceManager 구현.
// MediaMTX를 launchctl(LaunchAgent)로 관리한다 — 자식 프로세스 아님.
// 테스트/개발용; 배포 1순위는 WindowsRelayService(Windows SCM).
class LaunchdRelayService : public nv::app::IRelayServiceManager {
public:
    explicit LaunchdRelayService(
        std::string mediamtxBin = "/opt/homebrew/bin/mediamtx",
        std::string label       = "com.ziilab.newviewer.relay");

    // configPath로 LaunchAgent plist를 기록하고 launchctl로 기동(멱등). 성공 시 true.
    bool ensureRunning(const std::string& configPath) override;

    nv::app::RelayServiceStatus status() const override;

    // launchctl bootout + plist 삭제. 성공 시 true.
    bool stop() override;

private:
    std::string m_bin;
    std::string m_label;

    // ~/Library/LaunchAgents/<label>.plist 경로
    std::string plistPath() const;
};

} // namespace nv::infra

#endif // __APPLE__
