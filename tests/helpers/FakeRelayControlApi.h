#pragma once
#include <vector>
#include "src/app/ports/IRelayControlApi.h"

namespace nv::test {

// 테스트가 직접 헬스 데이터를 주입하고 pathsHealth()로 조회하게 한다.
class FakeRelayControlApi final : public nv::app::IRelayControlApi {
public:
    void setHealth(std::vector<nv::app::RelayPathHealth> health) {
        m_health = std::move(health);
    }

    std::vector<nv::app::RelayPathHealth> pathsHealth() override {
        return m_health;
    }

private:
    std::vector<nv::app::RelayPathHealth> m_health;
};

} // namespace nv::test
