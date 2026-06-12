#include <catch2/catch_test_macros.hpp>
#include "src/app/SnapshotService.h"
#include "tests/helpers/FakeLogger.h"
#include "tests/helpers/FakeSnapshotSink.h"

using namespace nv::app;
using namespace nv::test;

namespace {
struct Fixture {
    FakeSnapshotSink sink;
    FakeLogger       logger;
    SnapshotService  svc{sink, logger};
};
} // namespace

// ── capture: sink.snapshot 호출 + 경로 검증 ─────────────────────────────────

TEST_CASE("capture: sink.snapshot을 호출하고 channelId/경로를 전달한다") {
    Fixture f;
    const bool ok = f.svc.capture("ch1", "Camera1", "/tmp/ch1_snap.png");

    CHECK(ok);
    CHECK(f.sink.callCount == 1);
    REQUIRE(f.sink.calls.size() == 1);
    CHECK(f.sink.calls[0].channelId  == "ch1");
    CHECK(f.sink.calls[0].outputPath == "/tmp/ch1_snap.png");
}

TEST_CASE("capture: sink가 true 반환하면 capture도 true 반환") {
    Fixture f;
    f.sink.returnValue = true;
    CHECK(f.svc.capture("ch1", "Cam", "/tmp/a.png"));
}

TEST_CASE("capture: sink가 false 반환하면 capture도 false 반환 + 경고 로그") {
    Fixture f;
    f.sink.returnValue = false;

    const bool ok = f.svc.capture("ch1", "Cam", "/tmp/fail.png");

    CHECK_FALSE(ok);
    // 경고 로그가 기록되어야 한다
    const bool hasWarn = std::any_of(f.logger.entries.begin(), f.logger.entries.end(),
        [](const auto& e){ return e.level == nv::app::LogLevel::Warn; });
    CHECK(hasWarn);
}

TEST_CASE("capture: 여러 채널 스냅샷을 독립적으로 처리한다") {
    Fixture f;
    f.svc.capture("ch1", "Cam1", "/tmp/ch1.png");
    f.svc.capture("ch2", "Cam2", "/tmp/ch2.png");
    f.svc.capture("ch1", "Cam1", "/tmp/ch1_2.png");

    CHECK(f.sink.callCount == 3);
    CHECK(f.sink.calls[0].channelId == "ch1");
    CHECK(f.sink.calls[1].channelId == "ch2");
    CHECK(f.sink.calls[2].channelId == "ch1");
    CHECK(f.sink.calls[2].outputPath == "/tmp/ch1_2.png");
}

TEST_CASE("capture: sink 실패 시 channelId가 경고 로그에 포함된다") {
    Fixture f;
    f.sink.returnValue = false;
    f.svc.capture("channel_42", "MyCam", "/tmp/x.png");

    REQUIRE_FALSE(f.logger.entries.empty());
    const auto& e = f.logger.entries.front();
    CHECK(e.channelId == "channel_42");
    CHECK(e.level == nv::app::LogLevel::Warn);
}
