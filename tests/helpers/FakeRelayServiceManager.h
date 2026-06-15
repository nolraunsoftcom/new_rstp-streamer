#pragma once
#include <string>
#include "src/app/ports/IRelayServiceManager.h"

namespace nv::test {

// ensureRunning 호출을 기록하고 설정 가능한 결과를 반환한다.
class FakeRelayServiceManager final : public nv::app::IRelayServiceManager {
public:
    bool ensureRunning(const std::string& configPath) override {
        ++ensureRunningCalls;
        lastConfigPath = configPath;
        if (ensureRunningResult)
            m_status.running = true;
        return ensureRunningResult;
    }

    nv::app::RelayServiceStatus status() const override {
        return m_status;
    }

    bool stop() override {
        ++stopCalls;
        m_status.running = false;
        return true;
    }

    int ensureRunningCalls = 0;
    int stopCalls = 0;
    std::string lastConfigPath;
    bool ensureRunningResult = true;
    nv::app::RelayServiceStatus m_status;
};

} // namespace nv::test
