#include <catch2/catch_test_macros.hpp>
#include "src/domain/recording/RecordingState.h"
#include "tests/helpers/FakeRecordingSink.h"
#include "tests/helpers/FakeSnapshotSink.h"

using namespace nv::domain;
using namespace nv::test;

// ── RecordingState toString ──────────────────────────────────────────────────

TEST_CASE("RecordingState::toString 각 열거자 반환") {
    CHECK(toString(RecordingState::Idle)      == "Idle");
    CHECK(toString(RecordingState::Starting)  == "Starting");
    CHECK(toString(RecordingState::Recording) == "Recording");
}

// ── SegmentPolicy 기본값 ─────────────────────────────────────────────────────

TEST_CASE("SegmentPolicy 기본값: maxDuration=600s, splitOnReconnect=true") {
    SegmentPolicy p;
    CHECK(p.maxDuration == std::chrono::seconds{600});
    CHECK(p.splitOnReconnect == true);
}

TEST_CASE("SegmentPolicy 커스텀 값 설정 가능") {
    SegmentPolicy p{std::chrono::seconds{300}, false};
    CHECK(p.maxDuration == std::chrono::seconds{300});
    CHECK(p.splitOnReconnect == false);
}

// ── FakeRecordingSink 기본 동작 ──────────────────────────────────────────────

TEST_CASE("FakeRecordingSink: startRecording 호출을 기록한다") {
    FakeRecordingSink sink;
    bool ok = sink.startRecording("ch1", "/tmp/out.mkv");
    CHECK(ok);
    CHECK(sink.startCount == 1);
    REQUIRE(sink.startCalls.size() == 1);
    CHECK(sink.startCalls[0].channelId  == "ch1");
    CHECK(sink.startCalls[0].outputPath == "/tmp/out.mkv");
}

TEST_CASE("FakeRecordingSink: 이미 녹화 중이면 startRecording은 false 반환") {
    FakeRecordingSink sink;
    sink.startRecording("ch1", "/tmp/a.mkv");
    bool ok = sink.startRecording("ch1", "/tmp/b.mkv");
    CHECK_FALSE(ok);
    CHECK(sink.startCount == 1);  // 두 번째는 카운트되지 않음
}

TEST_CASE("FakeRecordingSink: stopRecording 호출을 기록한다") {
    FakeRecordingSink sink;
    sink.startRecording("ch1", "/tmp/out.mkv");
    sink.stopRecording("ch1");
    CHECK(sink.stopCount == 1);
    REQUIRE(sink.stopCalls.size() == 1);
    CHECK(sink.stopCalls[0] == "ch1");
    CHECK_FALSE(sink.isRecording("ch1"));
}

TEST_CASE("FakeRecordingSink: isRecording은 start/stop 상태를 반영한다") {
    FakeRecordingSink sink;
    CHECK_FALSE(sink.isRecording("ch1"));
    sink.startRecording("ch1", "/tmp/out.mkv");
    CHECK(sink.isRecording("ch1"));
    sink.stopRecording("ch1");
    CHECK_FALSE(sink.isRecording("ch1"));
}

TEST_CASE("FakeRecordingSink: 채널별 독립 녹화 상태") {
    FakeRecordingSink sink;
    sink.startRecording("ch1", "/tmp/ch1.mkv");
    sink.startRecording("ch2", "/tmp/ch2.mkv");
    CHECK(sink.isRecording("ch1"));
    CHECK(sink.isRecording("ch2"));
    sink.stopRecording("ch1");
    CHECK_FALSE(sink.isRecording("ch1"));
    CHECK(sink.isRecording("ch2"));
}

// ── FakeSnapshotSink 기본 동작 ───────────────────────────────────────────────

TEST_CASE("FakeSnapshotSink: snapshot 호출을 기록한다") {
    FakeSnapshotSink sink;
    bool ok = sink.snapshot("ch1", "/tmp/frame.png");
    CHECK(ok);
    CHECK(sink.callCount == 1);
    REQUIRE(sink.calls.size() == 1);
    CHECK(sink.calls[0].channelId  == "ch1");
    CHECK(sink.calls[0].outputPath == "/tmp/frame.png");
}

TEST_CASE("FakeSnapshotSink: returnValue=false 시 실패 반환") {
    FakeSnapshotSink sink;
    sink.returnValue = false;
    bool ok = sink.snapshot("ch1", "/tmp/frame.png");
    CHECK_FALSE(ok);
    CHECK(sink.callCount == 1);  // 호출 자체는 기록됨
}

TEST_CASE("FakeSnapshotSink: 여러 채널 스냅샷 호출 기록") {
    FakeSnapshotSink sink;
    sink.snapshot("ch1", "/tmp/ch1.png");
    sink.snapshot("ch2", "/tmp/ch2.png");
    sink.snapshot("ch1", "/tmp/ch1_2.png");
    CHECK(sink.callCount == 3);
    CHECK(sink.calls[0].channelId == "ch1");
    CHECK(sink.calls[1].channelId == "ch2");
    CHECK(sink.calls[2].channelId == "ch1");
}
