#include <catch2/catch_test_macros.hpp>
#include "src/app/ChannelController.h"
#include "tests/helpers/FakeClock.h"
#include "tests/helpers/FakeStreamSource.h"
#include "tests/helpers/FakeLogger.h"

using namespace nv::app;
using namespace nv::domain;
using namespace nv::test;
using namespace std::chrono_literals;

namespace {
struct Fixture {
    FakeClock clock;
    FakeStreamSource source;
    FakeLogger logger;
    ChannelController ctrl{"ch1", "rtsp://169.254.4.1:8900/live",
                           source, clock, logger,
                           ReconnectPolicy{}, StallPolicy{}};
};
}

TEST_CASE("connect()는 소스를 열고 상태가 Connecting이 된다") {
    Fixture f;
    f.ctrl.connect();
    CHECK(f.source.openCount == 1);
    CHECK(f.source.lastUrl == "rtsp://169.254.4.1:8900/live");
    CHECK(f.ctrl.state() == ConnState::Connecting);
}

TEST_CASE("세션 열림 → 패킷 → 표시: 진단 단계가 순서대로 Ok가 된다") {
    Fixture f;
    f.ctrl.connect();
    auto* l = f.source.listener();
    REQUIRE(l != nullptr);

    l->onSessionOpened();
    CHECK(f.ctrl.health().stageState(HealthStage::RtspSession) == StageState::Ok);
    CHECK(f.ctrl.health().stageState(HealthStage::PacketFlow) == StageState::Unknown);

    l->onPacketReceived();
    CHECK(f.ctrl.state() == ConnState::Streaming);
    CHECK(f.ctrl.health().stageState(HealthStage::PacketFlow) == StageState::Ok);

    l->onFrameDecoded();
    CHECK(f.ctrl.health().stageState(HealthStage::Decoding) == StageState::Ok);

    l->onFramePresented();
    CHECK(f.ctrl.health().stageState(HealthStage::Presenting) == StageState::Ok);
}

TEST_CASE("직결 모드: RelayIntake는 NotApplicable") {
    Fixture f;
    CHECK(f.ctrl.health().stageState(HealthStage::RelayIntake) == StageState::NotApplicable);
}

TEST_CASE("가짜 연결 시나리오: 세션 열림 후 침묵 → tick들이 재접속을 구동한다") {
    Fixture f;
    f.ctrl.connect();
    f.source.listener()->onSessionOpened();

    f.clock.advance(6s);
    f.ctrl.tick();                            // 가짜 판정 → CloseSource 실행됨
    CHECK(f.source.closeCount == 1);
    CHECK(f.ctrl.state() == ConnState::Reconnecting);
    CHECK(f.ctrl.health().stageState(HealthStage::PacketFlow) == StageState::Failed);
    CHECK(f.ctrl.health().failedReason() == DiagnosisReason::NoPackets);

    f.clock.advance(6s);
    f.ctrl.tick();                            // retryDelay 경과 → 재오픈
    CHECK(f.source.openCount == 2);
    CHECK(f.ctrl.state() == ConnState::Connecting);
    // 재접속 사이클 시작 시 health는 리셋된다 (RelayIntake 제외)
    CHECK(f.ctrl.health().stageState(HealthStage::RtspSession) == StageState::Unknown);
}

TEST_CASE("상태 변화는 구조화 로그로 남는다") {
    Fixture f;
    f.ctrl.connect();
    f.source.listener()->onSessionOpened();
    f.clock.advance(6s);
    f.ctrl.tick();
    bool found = false;
    for (auto& e : f.logger.entries) {
        if (e.channelId == "ch1" && e.reason == DiagnosisReason::NoPackets) found = true;
    }
    CHECK(found);
}

TEST_CASE("disconnect()는 소스를 닫고 Idle로") {
    Fixture f;
    f.ctrl.connect();
    f.ctrl.disconnect();
    CHECK(f.source.closeCount == 1);
    CHECK(f.ctrl.state() == ConnState::Idle);
}

TEST_CASE("늦은 소스 이벤트(close 이후)는 상태를 바꾸지 않는다") {
    Fixture f;
    f.ctrl.connect();
    auto* l = f.source.listener();
    f.ctrl.disconnect();
    l->onSessionOpened();                     // 죽은 소스에서 늦게 도착
    CHECK(f.ctrl.state() == ConnState::Idle);
}

// ────────────────────────────────────────────
// relay 모드 테스트
// ────────────────────────────────────────────
namespace {
struct RelayFixture {
    FakeClock clock;
    FakeStreamSource source;
    FakeLogger logger;
    ChannelController ctrl{"ch1", "rtsp://127.0.0.1:8554/ch1",
                           source, clock, logger,
                           ReconnectPolicy{}, StallPolicy{},
                           /*useRelay=*/true};
};
}

TEST_CASE("relay 모드: onRelayHealth(up=true) → RelayIntake=Ok") {
    RelayFixture f;
    f.ctrl.onRelayHealth(true, DiagnosisReason::None);
    CHECK(f.ctrl.health().stageState(HealthStage::RelayIntake) == StageState::Ok);
}

TEST_CASE("relay 모드: onRelayHealth(up=false, RelayNoSource) → RelayIntake=Failed, 사유 RelayNoSource") {
    RelayFixture f;
    f.ctrl.onRelayHealth(false, DiagnosisReason::RelayNoSource);
    CHECK(f.ctrl.health().stageState(HealthStage::RelayIntake) == StageState::Failed);
    CHECK(f.ctrl.health().failedReason() == DiagnosisReason::RelayNoSource);
}

TEST_CASE("relay 모드: Failed 상태에서 onRelayHealth(up=true) → sourceAvailableHint로 Connecting 전이") {
    RelayFixture f;
    f.ctrl.connect();
    // 가짜연결 사이클로 Failed 상태까지 구동
    while (f.ctrl.state() != ConnState::Failed) {
        if (f.source.listener()) f.source.listener()->onSessionOpened();
        f.clock.advance(6s);
        f.ctrl.tick();
        f.clock.advance(6s);
        f.ctrl.tick();
    }
    REQUIRE(f.ctrl.state() == ConnState::Failed);
    const int opensAtFailed = f.source.openCount;
    f.ctrl.onRelayHealth(true, DiagnosisReason::None);
    CHECK(f.ctrl.state() == ConnState::Connecting);
    CHECK(f.source.openCount == opensAtFailed + 1);
}

TEST_CASE("직결 모드: onRelayHealth 무동작, RelayIntake=NotApplicable 유지") {
    Fixture f;  // 직결 모드 (useRelay=false)
    CHECK(f.ctrl.health().stageState(HealthStage::RelayIntake) == StageState::NotApplicable);
    f.ctrl.onRelayHealth(true, DiagnosisReason::None);
    CHECK(f.ctrl.health().stageState(HealthStage::RelayIntake) == StageState::NotApplicable);
    f.ctrl.onRelayHealth(false, DiagnosisReason::RelayDown);
    CHECK(f.ctrl.health().stageState(HealthStage::RelayIntake) == StageState::NotApplicable);
}

TEST_CASE("D1: Failed 후 notifySourceAvailable → 즉시 재오픈 (relay 헬스 부활 경로)") {
    Fixture f;
    f.ctrl.connect();
    while (f.ctrl.state() != ConnState::Failed) {    // 가짜연결 사이클로 Failed까지
        if (f.source.listener()) f.source.listener()->onSessionOpened();
        f.clock.advance(6s);
        f.ctrl.tick();
        f.clock.advance(6s);
        f.ctrl.tick();
    }
    const int opensAtFailed = f.source.openCount;
    f.ctrl.notifySourceAvailable();
    CHECK(f.ctrl.state() == ConnState::Connecting);
    CHECK(f.source.openCount == opensAtFailed + 1);
}
