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

TEST_CASE("moveGrid: 빈 셀로 이동 — gridIndex만 변경, 교환 없음") {
    Fixture f;
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");   // grid 0
    const auto id2 = f.mgr.addChannel("b", "rtsp://b");   // grid 1
    f.mgr.moveGrid(id1, 5);                                // 빈 셀 5로 이동
    CHECK(f.mgr.config(id1).gridIndex == 5);
    CHECK(f.mgr.config(id2).gridIndex == 1);              // 무영향
}

TEST_CASE("moveGrid: 점유 셀로 이동 — 두 채널 위치 교환") {
    Fixture f;
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");   // grid 0
    const auto id2 = f.mgr.addChannel("b", "rtsp://b");   // grid 1
    f.mgr.moveGrid(id1, 1);                                // id2가 점유한 셀로
    CHECK(f.mgr.config(id1).gridIndex == 1);
    CHECK(f.mgr.config(id2).gridIndex == 0);              // 출발 셀로 밀려남(교환)
}

TEST_CASE("moveGrid: listIndex 불변(그리드와 독립)") {
    Fixture f;
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");   // grid 0, list 0
    f.mgr.moveGrid(id1, 3);
    CHECK(f.mgr.config(id1).gridIndex == 3);
    CHECK(f.mgr.config(id1).listIndex == 0);             // 리스트 순서 영향 없음
}

TEST_CASE("addChannel: listIndex도 gridIndex와 병렬로 자동 배치") {
    Fixture f;
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");
    const auto id2 = f.mgr.addChannel("b", "rtsp://b");
    CHECK(f.mgr.config(id1).listIndex == 0);
    CHECK(f.mgr.config(id2).listIndex == 1);
}

TEST_CASE("reorderList: listIndex만 재배열, gridIndex 불변(독립 순서)") {
    Fixture f;
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");   // grid 0, list 0
    const auto id2 = f.mgr.addChannel("b", "rtsp://b");   // grid 1, list 1
    const auto id3 = f.mgr.addChannel("c", "rtsp://c");   // grid 2, list 2

    // 새 리스트 순서: c, a, b
    f.mgr.reorderList({id3, id1, id2});
    CHECK(f.mgr.config(id3).listIndex == 0);
    CHECK(f.mgr.config(id1).listIndex == 1);
    CHECK(f.mgr.config(id2).listIndex == 2);

    // gridIndex는 그대로 — 그리드 순서와 리스트 순서가 독립
    CHECK(f.mgr.config(id1).gridIndex == 0);
    CHECK(f.mgr.config(id2).gridIndex == 1);
    CHECK(f.mgr.config(id3).gridIndex == 2);
}

TEST_CASE("reorderList: 부분 목록 — 빠진 채널은 기존 순서 보존해 뒤에 붙음") {
    Fixture f;
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");   // list 0
    const auto id2 = f.mgr.addChannel("b", "rtsp://b");   // list 1
    const auto id3 = f.mgr.addChannel("c", "rtsp://c");   // list 2

    // id3만 맨 앞으로 — 나머지(id1,id2)는 기존 listIndex 순서로 뒤에 안정 배치
    f.mgr.reorderList({id3});
    CHECK(f.mgr.config(id3).listIndex == 0);
    CHECK(f.mgr.config(id1).listIndex == 1);
    CHECK(f.mgr.config(id2).listIndex == 2);
}

TEST_CASE("reorderList: 저장 + 목록 변경 통지") {
    Fixture f;
    int notified = 0;
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");
    const auto id2 = f.mgr.addChannel("b", "rtsp://b");
    f.mgr.setListChangedObserver([&] { ++notified; });
    const int beforeSave = f.repo.saveCount;
    f.mgr.reorderList({id2, id1});
    CHECK(f.repo.saveCount == beforeSave + 1);
    CHECK(notified == 1);
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

// ── P1 신규 테스트 ─────────────────────────────────────────────────────────

TEST_CASE("setSnapshotObserver를 채널 생성 후 호출하면 기존 채널에도 rebind된다") {
    Fixture f;
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");
    const auto id2 = f.mgr.addChannel("b", "rtsp://b");

    std::vector<std::string> seen;
    f.mgr.setSnapshotObserver([&](const std::string& id, const ChannelSnapshot&) {
        seen.push_back(id);
    });

    f.mgr.connectAll();
    // 두 채널 모두 connect → Connecting 스냅샷이 콜백에 등장해야 한다
    CHECK(std::find(seen.begin(), seen.end(), id1) != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), id2) != seen.end());
}

TEST_CASE("setSnapshotObserver(nullptr) 후에는 콜백이 호출되지 않는다") {
    Fixture f;
    int count = 0;
    f.mgr.setSnapshotObserver([&](const std::string&, const ChannelSnapshot&) { ++count; });
    const auto id = f.mgr.addChannel("a", "rtsp://a");
    f.mgr.connectAll();
    const int countAfter = count;
    REQUIRE(countAfter > 0);

    f.mgr.setSnapshotObserver(nullptr);
    // 상태 변화 유발: source가 열렸으므로 onSessionOpened 이벤트 주입
    f.factory.registry.at(id)->listener()->onSessionOpened();
    CHECK(count == countAfter);   // 옵저버 해제 후 카운트 불변
}

TEST_CASE("disconnectAll은 전 채널 소스를 닫는다(유령 방지 핵심)") {
    Fixture f;
    const auto id1 = f.mgr.addChannel("a", "rtsp://a");
    const auto id2 = f.mgr.addChannel("b", "rtsp://b");
    const auto id3 = f.mgr.addChannel("c", "rtsp://c");
    f.mgr.connectAll();
    REQUIRE(f.factory.registry.at(id1)->openCount >= 1);

    f.mgr.disconnectAll();
    CHECK(f.factory.registry.at(id1)->closeCount >= 1);
    CHECK(f.factory.registry.at(id2)->closeCount >= 1);
    CHECK(f.factory.registry.at(id3)->closeCount >= 1);
    CHECK(f.mgr.controller(id1)->state() == ConnState::Idle);
    CHECK(f.mgr.controller(id2)->state() == ConnState::Idle);
    CHECK(f.mgr.controller(id3)->state() == ConnState::Idle);
}

TEST_CASE("setListChangedObserver(nullptr) 후 add/remove가 통지 안 함") {
    Fixture f;
    int count = 0;
    f.mgr.setListChangedObserver([&] { ++count; });
    const auto id = f.mgr.addChannel("a", "rtsp://a");
    REQUIRE(count == 1);

    f.mgr.setListChangedObserver(nullptr);
    f.mgr.addChannel("b", "rtsp://b");
    f.mgr.removeChannel(id);
    CHECK(count == 1);   // nullptr 설정 후 통지 없음
}

TEST_CASE("addChannel: rtsp 아닌 스킴(file://, http://)은 거부") {
    Fixture f;
    CHECK(f.mgr.addChannel("a", "file:///etc/passwd").empty());
    CHECK(f.mgr.addChannel("b", "http://example.com/stream").empty());
    CHECK(f.mgr.channelCount() == 0);
    // rtsp:// 는 허용
    CHECK_FALSE(f.mgr.addChannel("c", "rtsp://cam1").empty());
    CHECK(f.mgr.channelCount() == 1);
}

// ── F4: restore 경로 URL 검증 ─────────────────────────────────────────────

TEST_CASE("restore: 부적합 URL(http://) 항목은 스킵, 유효 항목만 복원") {
    FakeChannelRepository repo;
    repo.stored = {
        {"ch1", "valid",   "rtsp://cam1",            0},
        {"ch2", "invalid", "http://example.com/bad", 1},
        {"ch3", "valid2",  "rtsp://cam2",            2},
    };
    FakeRuntimeFactory factory;
    FakeClock clock;
    FakeLogger logger;
    ChannelManager mgr{repo, factory, clock, logger, ReconnectPolicy{}, StallPolicy{}};

    mgr.restore(false);
    CHECK(mgr.channelCount() == 2);
    CHECK(factory.registry.count("ch1") == 1);
    CHECK(factory.registry.count("ch2") == 0);  // http:// 스킵됨
    CHECK(factory.registry.count("ch3") == 1);
}

// ── F6: isValidRtspUrl 대소문자 무시 ─────────────────────────────────────

TEST_CASE("addChannel: 대문자 스킴 RTSP://, RTSPS://도 허용") {
    Fixture f;
    CHECK_FALSE(f.mgr.addChannel("a", "RTSP://cam1").empty());
    CHECK_FALSE(f.mgr.addChannel("b", "RTSPS://cam2").empty());
    CHECK_FALSE(f.mgr.addChannel("c", "Rtsp://cam3").empty());
    CHECK(f.mgr.channelCount() == 3);
}

// ── autoConnect 회귀 테스트 ────────────────────────────────────────────────

TEST_CASE("addChannel(autoConnect=true)는 cfg에 저장되고 저장소에 영속된다") {
    Fixture f;
    const auto id = f.mgr.addChannel("a", "rtsp://a", true);
    REQUIRE_FALSE(id.empty());
    CHECK(f.mgr.config(id).autoConnect == true);
    REQUIRE(f.repo.stored.size() == 1);
    CHECK(f.repo.stored[0].autoConnect == true);
}

TEST_CASE("addChannel(autoConnect=false, 기본값)는 cfg에 false로 저장된다") {
    Fixture f;
    const auto id = f.mgr.addChannel("b", "rtsp://b");
    REQUIRE_FALSE(id.empty());
    CHECK(f.mgr.config(id).autoConnect == false);
    REQUIRE(f.repo.stored.size() == 1);
    CHECK(f.repo.stored[0].autoConnect == false);
}

TEST_CASE("restore: 전역 autoConnect=false여도 cfg.autoConnect=true 채널은 연결된다") {
    FakeChannelRepository repo;
    repo.stored = {
        {"ch1", "auto",   "rtsp://a", 0, /*listIndex=*/0, true},   // autoConnect=true
        {"ch2", "manual", "rtsp://b", 1, /*listIndex=*/1, false},   // autoConnect=false
    };
    FakeRuntimeFactory factory;
    FakeClock clock;
    FakeLogger logger;
    ChannelManager mgr{repo, factory, clock, logger, ReconnectPolicy{}, StallPolicy{}};

    mgr.restore(false);   // 전역 autoConnect=false
    CHECK(mgr.channelCount() == 2);
    CHECK(factory.registry.at("ch1")->openCount == 1);   // 채널별 true → 연결됨
    CHECK(factory.registry.at("ch2")->openCount == 0);   // 채널별 false → 연결 안됨
}

TEST_CASE("restore: 전역 autoConnect=true면 채널 플래그 무관하게 모두 연결된다") {
    FakeChannelRepository repo;
    repo.stored = {
        {"ch1", "auto",   "rtsp://a", 0, /*listIndex=*/0, true},
        {"ch2", "manual", "rtsp://b", 1, /*listIndex=*/1, false},
    };
    FakeRuntimeFactory factory;
    FakeClock clock;
    FakeLogger logger;
    ChannelManager mgr{repo, factory, clock, logger, ReconnectPolicy{}, StallPolicy{}};

    mgr.restore(true);   // 전역 autoConnect=true → 모두 연결
    CHECK(mgr.channelCount() == 2);
    CHECK(factory.registry.at("ch1")->openCount == 1);
    CHECK(factory.registry.at("ch2")->openCount == 1);
}

TEST_CASE("updateChannel로 autoConnect 토글이 cfg와 저장소에 반영된다") {
    Fixture f;
    const auto id = f.mgr.addChannel("a", "rtsp://a", false);
    REQUIRE_FALSE(id.empty());
    CHECK(f.mgr.config(id).autoConnect == false);

    f.mgr.updateChannel(id, "a", "rtsp://a", true);
    CHECK(f.mgr.config(id).autoConnect == true);
    REQUIRE_FALSE(f.repo.stored.empty());
    CHECK(f.repo.stored[0].autoConnect == true);

    f.mgr.updateChannel(id, "a", "rtsp://a", false);
    CHECK(f.mgr.config(id).autoConnect == false);
    CHECK(f.repo.stored[0].autoConnect == false);
}
