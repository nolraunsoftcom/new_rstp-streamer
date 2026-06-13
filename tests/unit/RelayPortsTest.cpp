#include <catch2/catch_test_macros.hpp>
#include "tests/helpers/FakeRelayServiceManager.h"
#include "tests/helpers/FakeRelayControlApi.h"

using namespace nv::app;
using namespace nv::test;

TEST_CASE("relay 페이크: ServiceManager 호출 기록") {
    FakeRelayServiceManager svc;
    CHECK(svc.ensureRunning("/tmp/x.yml"));
    CHECK(svc.ensureRunningCalls == 1);
    CHECK(svc.lastConfigPath == "/tmp/x.yml");
    CHECK(svc.status().running);
    CHECK(svc.stop());
    CHECK_FALSE(svc.status().running);
}

TEST_CASE("relay 페이크: ControlApi 헬스 설정/조회") {
    FakeRelayControlApi api;
    api.setHealth({{"ch1", true, true}, {"ch2", false, false}});
    auto h = api.pathsHealth();
    REQUIRE(h.size() == 2);
    CHECK(h[0].name == "ch1"); CHECK(h[0].hasSource);
    CHECK_FALSE(h[1].hasSource);
}
