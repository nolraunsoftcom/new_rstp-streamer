#include <catch2/catch_test_macros.hpp>
#include "src/app/RelaySupervisor.h"
#include "tests/helpers/FakeRelayServiceManager.h"
#include "tests/helpers/FakeRelayControlApi.h"
#include "tests/helpers/FakeLogger.h"

using namespace nv::app;
using namespace nv::domain;
using namespace nv::test;

static std::vector<RelayPath> oneRelay() {
    return {{"ch1", "rtsp://169.254.4.1:8900/live", true}};
}

TEST_CASE("RelaySupervisor: ensureUp = 생성→검증→기록→기동요청") {
    FakeRelayServiceManager svc;
    FakeRelayControlApi api;
    FakeLogger log;
    std::string wYml, wPath;
    RelaySupervisor sup(svc, api, log,
        [&](const std::string& p, const std::string& y) {
            wPath = p; wYml = y; return true;
        });
    REQUIRE(sup.ensureUp(oneRelay(), "/tmp/mediamtx.yml"));
    CHECK(wPath == "/tmp/mediamtx.yml");
    CHECK(wYml.find("sourceOnDemand: no") != std::string::npos);
    CHECK(svc.ensureRunningCalls == 1);
    CHECK(svc.lastConfigPath == "/tmp/mediamtx.yml");
}

TEST_CASE("RelaySupervisor: restart = stop 후 재기동(self-heal)") {
    FakeRelayServiceManager svc;
    FakeRelayControlApi api;
    FakeLogger log;
    RelaySupervisor sup(svc, api, log, [](auto&, auto&) { return true; });
    REQUIRE(sup.restart(oneRelay(), "/tmp/mediamtx.yml"));
    CHECK(svc.stopCalls == 1);              // 먼저 완전 정리(bootout)
    CHECK(svc.ensureRunningCalls == 1);     // 그 다음 재기동
    CHECK(svc.lastConfigPath == "/tmp/mediamtx.yml");
}

TEST_CASE("RelaySupervisor: writer 실패 시 서비스 기동 안 함") {
    FakeRelayServiceManager svc;
    FakeRelayControlApi api;
    FakeLogger log;
    RelaySupervisor sup(svc, api, log,
        [](const std::string&, const std::string&) { return false; });
    CHECK_FALSE(sup.ensureUp(oneRelay(), "/tmp/x.yml"));
    CHECK(svc.ensureRunningCalls == 0);
}

TEST_CASE("RelaySupervisor: pollHealth hasSource=true → up/None") {
    FakeRelayServiceManager svc;
    FakeRelayControlApi api;
    FakeLogger log;
    api.setHealth({{"ch1", true, true}});
    RelaySupervisor sup(svc, api, log, [](auto&, auto&) { return true; });
    auto h = sup.pollHealth(oneRelay());
    REQUIRE(h.count("ch1") == 1);
    CHECK(h["ch1"].up);
    CHECK(h["ch1"].reason == DiagnosisReason::None);
}

TEST_CASE("RelaySupervisor: pollHealth hasSource=false → RelayNoSource") {
    FakeRelayServiceManager svc;
    FakeRelayControlApi api;
    FakeLogger log;
    api.setHealth({{"ch1", false, false}});
    RelaySupervisor sup(svc, api, log, [](auto&, auto&) { return true; });
    auto h = sup.pollHealth(oneRelay());
    CHECK_FALSE(h["ch1"].up);
    CHECK(h["ch1"].reason == DiagnosisReason::RelayNoSource);
}

TEST_CASE("RelaySupervisor: pollHealth API 무응답(빈벡터) → RelayDown") {
    FakeRelayServiceManager svc;
    FakeRelayControlApi api;
    FakeLogger log;  // setHealth 안 함 = 빈 벡터
    RelaySupervisor sup(svc, api, log, [](auto&, auto&) { return true; });
    auto h = sup.pollHealth(oneRelay());
    CHECK_FALSE(h["ch1"].up);
    CHECK(h["ch1"].reason == DiagnosisReason::RelayDown);
}

TEST_CASE("RelaySupervisor: 직결 채널은 pollHealth 결과에 없음") {
    FakeRelayServiceManager svc;
    FakeRelayControlApi api;
    FakeLogger log;
    api.setHealth({{"ch1", true, true}});
    RelaySupervisor sup(svc, api, log, [](auto&, auto&) { return true; });
    std::vector<RelayPath> mixed{{"ch1", "rtsp://d", true}, {"ch2", "rtsp://d2", false}};
    auto h = sup.pollHealth(mixed);
    CHECK(h.count("ch1") == 1);
    CHECK(h.count("ch2") == 0);
}
