#pragma once
#include <string>

namespace nv::app {

struct RelayServiceStatus { bool installed=false; bool running=false; std::string detail; };

// MediaMTX를 OS 서비스로 관리(자식 프로세스 아님). 플랫폼 어댑터가 구현.
class IRelayServiceManager {
public:
    virtual ~IRelayServiceManager() = default;

    // configPath로 서비스를 보장: 미설치면 설치, 미기동이면 기동. 멱등. 성공 시 true.
    virtual bool ensureRunning(const std::string& configPath) = 0;
    virtual RelayServiceStatus status() const = 0;
    virtual bool stop() = 0;   // 테스트/정리용
};

} // namespace nv::app
