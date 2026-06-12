#include <catch2/catch_test_macros.hpp>
#include "src/app/RecordingController.h"
#include "src/domain/recording/RecordingState.h"
#include "tests/helpers/FakeClock.h"
#include "tests/helpers/FakeLogger.h"
#include "tests/helpers/FakeRecordingSink.h"

using namespace nv::app;
using namespace nv::domain;
using namespace nv::test;
using namespace std::chrono_literals;

// FakeRecordingSink에 "다음 startRecording을 실패시키는" 기능이 없으므로
// 로컬에서 확장한다.
namespace {

class FailingRecordingSink final : public nv::app::IRecordingSink {
public:
    bool startRecording(const std::string& channelId,
                        const std::string& /*outputPath*/) override {
        if (failNext) { failNext = false; return false; }
        m_recording[channelId] = true;
        ++startCount;
        return true;
    }
    void stopRecording(const std::string& channelId) override {
        m_recording[channelId] = false;
        ++stopCount;
    }
    bool isRecording(const std::string& channelId) const override {
        auto it = m_recording.find(channelId);
        return it != m_recording.end() && it->second;
    }

    bool failNext  = false;
    int  startCount = 0;
    int  stopCount  = 0;
private:
    std::unordered_map<std::string, bool> m_recording;
};

struct Fixture {
    FakeClock            clock;
    FakeRecordingSink    sink;
    FakeLogger           logger;
    SegmentPolicy        policy{std::chrono::seconds{600}, true};
    RecordingController  ctrl{sink, clock, logger, policy};
};

} // namespace

// ── toggle: Idle → Recording ─────────────────────────────────────────────────

TEST_CASE("toggle: Idle 상태에서 toggle하면 startRecording 호출 + Recording 상태") {
    Fixture f;
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Idle);

    f.ctrl.toggle("ch1", "Camera1");

    CHECK(f.sink.startCount == 1);
    REQUIRE(f.sink.startCalls.size() == 1);
    CHECK(f.sink.startCalls[0].channelId == "ch1");
    CHECK_FALSE(f.sink.startCalls[0].outputPath.empty());   // 경로 비어있지 않음
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Recording);
}

// ── toggle: Recording → Idle ─────────────────────────────────────────────────

TEST_CASE("toggle: Recording 상태에서 toggle하면 stopRecording 호출 + Idle 상태") {
    Fixture f;
    f.ctrl.toggle("ch1", "Camera1");   // start
    REQUIRE(f.ctrl.stateOf("ch1") == RecordingState::Recording);

    f.ctrl.toggle("ch1", "Camera1");   // stop

    CHECK(f.sink.stopCount == 1);
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Idle);
}

// ── toggle: 채널별 독립 ──────────────────────────────────────────────────────

TEST_CASE("toggle: 두 채널이 독립적으로 녹화 상태를 갖는다") {
    Fixture f;
    f.ctrl.toggle("ch1", "Cam1");
    f.ctrl.toggle("ch2", "Cam2");

    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Recording);
    CHECK(f.ctrl.stateOf("ch2") == RecordingState::Recording);

    f.ctrl.toggle("ch1", "Cam1");  // ch1만 중지
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Idle);
    CHECK(f.ctrl.stateOf("ch2") == RecordingState::Recording);
}

// ── onReconnect: 녹화 중 세그먼트 분리 ──────────────────────────────────────

TEST_CASE("onReconnect: 녹화 중인 채널은 stop+start 2회 호출(세그먼트 분리)") {
    Fixture f;
    f.ctrl.toggle("ch1", "Cam1");   // start → startCount=1
    REQUIRE(f.ctrl.stateOf("ch1") == RecordingState::Recording);

    f.ctrl.onReconnect("ch1", "Cam1");

    // stop 1회 + start 1회(새 세그먼트) → stopCount=1, startCount=2
    CHECK(f.sink.stopCount  == 1);
    CHECK(f.sink.startCount == 2);
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Recording);
}

TEST_CASE("onReconnect: 녹화 중이지 않은 채널은 아무 동작 안 함") {
    Fixture f;
    f.ctrl.toggle("ch2", "Cam2");   // ch2만 시작

    f.ctrl.onReconnect("ch1", "Cam1");  // ch1은 Idle — 무동작

    CHECK(f.sink.stopCount  == 0);
    CHECK(f.sink.startCount == 1);   // ch2 start만
}

TEST_CASE("onReconnect: splitOnReconnect=false이면 아무 동작 안 함") {
    FakeClock clock;
    FakeRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy noSplit{std::chrono::seconds{600}, false};
    RecordingController ctrl{sink, clock, logger, noSplit};

    ctrl.toggle("ch1", "Cam1");
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);

    ctrl.onReconnect("ch1", "Cam1");

    CHECK(sink.stopCount == 0);
    CHECK(sink.startCount == 1);   // 최초 start만
}

// ── tick: maxDuration 초과 롤오버 ───────────────────────────────────────────

TEST_CASE("tick: maxDuration 초과 시 세그먼트 롤오버(stop+start)") {
    FakeClock clock;
    FakeRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{60}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    ctrl.toggle("ch1", "Cam1");
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);
    CHECK(sink.startCount == 1);

    // maxDuration - 1초: 롤오버 없음
    clock.advance(std::chrono::seconds{59});
    ctrl.tick();
    CHECK(sink.startCount == 1);
    CHECK(sink.stopCount  == 0);

    // maxDuration 도달
    clock.advance(std::chrono::seconds{1});
    ctrl.tick();
    CHECK(sink.stopCount  == 1);
    CHECK(sink.startCount == 2);
    CHECK(ctrl.stateOf("ch1") == RecordingState::Recording);
}

TEST_CASE("tick: Idle 채널은 롤오버하지 않는다") {
    FakeClock clock;
    FakeRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{60}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    // ch1 녹화 시작 안 함
    clock.advance(std::chrono::seconds{120});
    ctrl.tick();

    CHECK(sink.startCount == 0);
    CHECK(sink.stopCount  == 0);
}

// ── D8 격리: startRecording 실패 시 Idle 유지, 다른 채널 무영향 ──────────────

TEST_CASE("격리(D8): sink.startRecording이 false 반환 시 해당 채널 Idle 유지") {
    FakeClock clock;
    FailingRecordingSink sink;
    FakeLogger logger;
    RecordingController ctrl{sink, clock, logger};

    sink.failNext = true;
    ctrl.toggle("ch1", "Cam1");

    CHECK(ctrl.stateOf("ch1") == RecordingState::Idle);
    CHECK(sink.startCount == 0);   // 성공적 start 없음

    // 경고 로그가 남아야 한다
    const bool hasWarn = std::any_of(logger.entries.begin(), logger.entries.end(),
        [](const auto& e){ return e.level == nv::app::LogLevel::Warn; });
    CHECK(hasWarn);
}

TEST_CASE("격리(D8): ch1 startRecording 실패가 ch2 녹화에 영향 없음") {
    FakeClock clock;
    FailingRecordingSink sink;
    FakeLogger logger;
    RecordingController ctrl{sink, clock, logger};

    sink.failNext = true;
    ctrl.toggle("ch1", "Cam1");   // 실패 → ch1 Idle
    ctrl.toggle("ch2", "Cam2");   // 성공 → ch2 Recording

    CHECK(ctrl.stateOf("ch1") == RecordingState::Idle);
    CHECK(ctrl.stateOf("ch2") == RecordingState::Recording);
    CHECK(sink.startCount == 1);   // ch2 start만
}

// ── onChannelRemoved: 유령 Recording 상태 방지 ───────────────────────────────

TEST_CASE("onChannelRemoved: 녹화 중인 채널 삭제 시 Idle로 정리 + sink.stop 호출") {
    Fixture f;
    f.ctrl.toggle("ch1", "Cam1");   // Recording 상태 진입
    REQUIRE(f.ctrl.stateOf("ch1") == RecordingState::Recording);

    f.ctrl.onChannelRemoved("ch1");

    CHECK(f.sink.stopCount == 1);
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Idle);
}

TEST_CASE("onChannelRemoved: 삭제 후 tick에서 해당 채널 무처리(유령 없음)") {
    FakeClock clock;
    FakeRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{60}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    ctrl.toggle("ch1", "Cam1");
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);

    ctrl.onChannelRemoved("ch1");
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Idle);

    // maxDuration 초과 후 tick — 삭제된 채널이므로 롤오버 없음
    clock.advance(std::chrono::seconds{120});
    const int stopsBefore = sink.stopCount;
    ctrl.tick();
    CHECK(sink.stopCount == stopsBefore);   // 추가 stop 없음
    CHECK(sink.startCount == 1);            // 최초 start만
}

TEST_CASE("onChannelRemoved: Idle 채널 삭제 시 아무 동작 안 함") {
    Fixture f;
    // ch1은 녹화 안 함 — Idle 상태
    f.ctrl.onChannelRemoved("ch1");

    CHECK(f.sink.stopCount == 0);
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Idle);
}

TEST_CASE("onChannelRemoved: ch1 삭제가 ch2 녹화에 영향 없음") {
    Fixture f;
    f.ctrl.toggle("ch1", "Cam1");
    f.ctrl.toggle("ch2", "Cam2");
    REQUIRE(f.ctrl.stateOf("ch1") == RecordingState::Recording);
    REQUIRE(f.ctrl.stateOf("ch2") == RecordingState::Recording);

    f.ctrl.onChannelRemoved("ch1");

    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Idle);
    CHECK(f.ctrl.stateOf("ch2") == RecordingState::Recording);
    CHECK(f.sink.stopCount == 1);   // ch1 stop만
}

// ── 옵저버 통지 ──────────────────────────────────────────────────────────────

TEST_CASE("옵저버: toggle 시 상태 변화를 통지한다") {
    Fixture f;

    std::vector<std::pair<std::string, RecordingState>> events;
    f.ctrl.setObserver([&](const std::string& id, RecordingState s) {
        events.push_back({id, s});
    });

    f.ctrl.toggle("ch1", "Cam1");   // Idle → Recording
    f.ctrl.toggle("ch1", "Cam1");   // Recording → Idle

    REQUIRE(events.size() == 2);
    CHECK(events[0].first  == "ch1");
    CHECK(events[0].second == RecordingState::Recording);
    CHECK(events[1].first  == "ch1");
    CHECK(events[1].second == RecordingState::Idle);
}
