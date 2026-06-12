#include <catch2/catch_test_macros.hpp>
#include "src/app/ChannelManager.h"
#include "tests/helpers/FakeChannelRepository.h"
#include "tests/helpers/FakeRuntimeFactory.h"
#include "tests/helpers/FakeClock.h"
#include "tests/helpers/FakeLogger.h"

using namespace nv::app;
using namespace nv::domain;
using namespace nv::test;
using namespace std::chrono_literals;

namespace {
struct Fixture {
    FakeChannelRepository repo;
    FakeRuntimeFactory factory;
    FakeClock clock;
    FakeLogger logger;
    ChannelManager mgr{repo, factory, clock, logger, ReconnectPolicy{}, StallPolicy{}};
};
}

TEST_CASE("addChannel: id 부여, gridIndex 자동 배치, 즉시 저장") {
    Fixture f;
    const auto id1 = f.mgr.addChannel("카메라1", "rtsp://a");
    const auto id2 = f.mgr.addChannel("카메라2", "rtsp://b");
    CHECK(id1 != id2);
    REQUIRE(f.repo.saveCount == 2);
    REQUIRE(f.repo.stored.size() == 2);
    CHECK(f.repo.stored[0].gridIndex == 0);
    CHECK(f.repo.stored[1].gridIndex == 1);
    CHECK(f.factory.createCount == 2);
}

TEST_CASE("restore: 저장된 채널 복원 + autoConnect 시 전 채널 연결") {
    FakeChannelRepository repo;
    repo.stored = {{"ch1", "a", "rtsp://a", 0}, {"ch2", "b", "rtsp://b", 1}};
    FakeRuntimeFactory factory;
    FakeClock clock;
    FakeLogger logger;
    ChannelManager mgr{repo, factory, clock, logger, ReconnectPolicy{}, StallPolicy{}};

    mgr.restore(true);
    CHECK(mgr.channelCount() == 2);
    CHECK(factory.registry.at("ch1")->openCount == 1);
    CHECK(factory.registry.at("ch2")->openCount == 1);
    // 복원 후 새 채널 id는 기존과 충돌하지 않아야 한다
    const auto id3 = mgr.addChannel("c", "rtsp://c");
    CHECK(id3 == "ch3");
}

TEST_CASE("removeChannel: 해당 채널만 정리, 다른 채널 무영향 (R2 채널 독립)") {
    Fixture f;
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");
    const auto id2 = f.mgr.addChannel("b", "rtsp://b");
    f.mgr.connectAll();
    auto* src2 = f.factory.registry.at(id2);
    const int openBefore = src2->openCount;
    const int closeBefore = src2->closeCount;

    f.mgr.removeChannel(id1);
    CHECK(f.mgr.channelCount() == 1);
    CHECK(f.factory.destroyCount == 1);
    CHECK(src2->openCount == openBefore);    // id2는 건드리지 않았다
    CHECK(src2->closeCount == closeBefore);
    CHECK(f.repo.stored.size() == 1);
    CHECK(f.repo.stored[0].id == id2);
}

TEST_CASE("updateChannel: 이름/URL 변경 저장, 연결 중이면 새 URL로 재연결") {
    Fixture f;
    const auto id = f.mgr.addChannel("a", "rtsp://old");
    f.mgr.connectAll();
    auto* src = f.factory.registry.at(id);
    REQUIRE(src->lastUrl == "rtsp://old");

    f.mgr.updateChannel(id, "a2", "rtsp://new");
    CHECK(f.repo.stored[0].name == "a2");
    CHECK(f.repo.stored[0].url == "rtsp://new");
    CHECK(src->lastUrl == "rtsp://new");      // disconnect→setUrl→connect 경유
}

TEST_CASE("swapGrid: 두 채널의 gridIndex 교환 + 저장") {
    Fixture f;
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");   // grid 0
    const auto id2 = f.mgr.addChannel("b", "rtsp://b");   // grid 1
    f.mgr.swapGrid(id1, id2);
    CHECK(f.mgr.config(id1).gridIndex == 1);
    CHECK(f.mgr.config(id2).gridIndex == 0);
    CHECK(f.repo.saveCount == 3);
}

TEST_CASE("tickAll은 모든 채널 컨트롤러에 tick을 전달한다") {
    Fixture f;
    const auto id = f.mgr.addChannel("a", "rtsp://a");
    f.mgr.connectAll();
    f.factory.registry.at(id)->listener()->onSessionOpened();
    f.clock.advance(6s);
    f.mgr.tickAll();                          // 가짜연결 판정 발동
    CHECK(f.mgr.controller(id)->state() == ConnState::Reconnecting);
}

TEST_CASE("목록 변경 옵저버: add/remove/swap에서 통지") {
    Fixture f;
    int notified = 0;
    f.mgr.setListChangedObserver([&] { ++notified; });
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");
    const auto id2 = f.mgr.addChannel("b", "rtsp://b");
    f.mgr.swapGrid(id1, id2);
    f.mgr.removeChannel(id1);
    CHECK(notified == 4);
}

TEST_CASE("소프트 한도: maxChannels 초과 addChannel은 빈 id 반환 + 무동작") {
    FakeChannelRepository repo;
    FakeRuntimeFactory factory;
    FakeClock clock;
    FakeLogger logger;
    ChannelManager small{repo, factory, clock, logger,
                         ReconnectPolicy{}, StallPolicy{}, /*maxChannels=*/2};
    small.addChannel("a", "rtsp://a");
    small.addChannel("b", "rtsp://b");
    CHECK(small.channelCount() == 2);
    CHECK(small.addChannel("over", "rtsp://y").empty());
    CHECK(small.channelCount() == 2);
}

TEST_CASE("기본 한도는 32 (성능 기준선 20과 별개)") {
    Fixture f;
    for (int i = 0; i < 32; ++i) CHECK_FALSE(f.mgr.addChannel("c", "rtsp://x").empty());
    CHECK(f.mgr.addChannel("over", "rtsp://y").empty());
    CHECK(f.mgr.channelCount() == 32);
}

TEST_CASE("setSnapshotObserver: 채널ID와 함께 스냅샷이 전달된다") {
    Fixture f;
    std::vector<std::string> seen;
    f.mgr.setSnapshotObserver([&](const std::string& id, const ChannelSnapshot&) {
        seen.push_back(id);
    });
    const auto id = f.mgr.addChannel("a", "rtsp://a");
    f.mgr.connectAll();
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.back() == id);
}
