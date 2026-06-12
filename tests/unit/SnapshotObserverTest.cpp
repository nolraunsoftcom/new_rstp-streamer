#include <catch2/catch_test_macros.hpp>
#include "src/app/ChannelController.h"
#include "tests/helpers/FakeClock.h"
#include "tests/helpers/FakeStreamSource.h"
#include "tests/helpers/FakeLogger.h"

using namespace nv::app;
using namespace nv::domain;
using namespace nv::test;
using namespace std::chrono_literals;

TEST_CASE("상태/진단이 바뀔 때만 옵저버가 호출된다") {
    FakeClock clock;
    FakeStreamSource source;
    FakeLogger logger;
    ChannelController ctrl{"ch1", "rtsp://u", source, clock, logger,
                           ReconnectPolicy{}, StallPolicy{}};
    std::vector<ChannelSnapshot> snaps;
    ctrl.setObserver([&](const ChannelSnapshot& s) { snaps.push_back(s); });

    ctrl.connect();
    REQUIRE_FALSE(snaps.empty());
    CHECK(snaps.back().state == ConnState::Connecting);

    const auto countAfterConnect = snaps.size();
    ctrl.tick();                              // 아무 변화 없음 → 옵저버 호출 없음
    CHECK(snaps.size() == countAfterConnect);

    source.listener()->onSessionOpened();
    CHECK(snaps.size() > countAfterConnect);
    CHECK(snaps.back().state == ConnState::SessionOpen);
    CHECK(snaps.back().health.stageState(HealthStage::RtspSession) == StageState::Ok);
}

TEST_CASE("tick은 패킷 수신률과 마지막 수신 경과를 스냅샷에 싣는다") {
    FakeClock clock;
    FakeStreamSource source;
    FakeLogger logger;
    ChannelController ctrl{"ch1", "rtsp://u", source, clock, logger,
                           ReconnectPolicy{}, StallPolicy{}};
    std::vector<ChannelSnapshot> snaps;
    ctrl.setObserver([&](const ChannelSnapshot& s) { snaps.push_back(s); });

    ctrl.connect();
    source.listener()->onSessionOpened();
    for (int i = 0; i < 30; ++i) source.listener()->onPacketReceived();
    clock.advance(1s);
    ctrl.tick();
    REQUIRE_FALSE(snaps.empty());
    CHECK(snaps.back().packetsPerSec == 30.0);
    CHECK(snaps.back().msSinceLastPacket == 1000);

    clock.advance(3s);          // 수신 두절 3초
    ctrl.tick();
    CHECK(snaps.back().packetsPerSec == 0.0);
    CHECK(snaps.back().msSinceLastPacket == 4000);
}

TEST_CASE("수신 이력이 없으면 msSinceLastPacket은 -1") {
    FakeClock clock;
    FakeStreamSource source;
    FakeLogger logger;
    ChannelController ctrl{"ch1", "rtsp://u", source, clock, logger,
                           ReconnectPolicy{}, StallPolicy{}};
    std::vector<ChannelSnapshot> snaps;
    ctrl.setObserver([&](const ChannelSnapshot& s) { snaps.push_back(s); });
    ctrl.connect();
    CHECK(snaps.back().msSinceLastPacket == -1);
}

TEST_CASE("setUrl은 Idle에서만 적용된다") {
    FakeClock clock;
    FakeStreamSource source;
    FakeLogger logger;
    ChannelController ctrl{"ch1", "rtsp://old", source, clock, logger,
                           ReconnectPolicy{}, StallPolicy{}};
    ctrl.setUrl("rtsp://new");
    ctrl.connect();
    CHECK(source.lastUrl == "rtsp://new");

    ctrl.setUrl("rtsp://ignored");            // Connecting 중에는 무시
    ctrl.disconnect();
    ctrl.connect();
    CHECK(source.lastUrl == "rtsp://new");
}
