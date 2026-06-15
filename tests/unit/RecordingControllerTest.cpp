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
    bool hasRecordingError(const std::string& /*channelId*/) const override { return false; }

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

// ── onReconnect(드롭) + onStreaming(복구): 녹화 생존 ────────────────────────

TEST_CASE("생존: armed 채널의 드롭(onReconnect)은 세그먼트만 종료(Idle)+armed 유지, 복구(onStreaming)는 새 세그먼트") {
    Fixture f;
    f.ctrl.toggle("ch1", "Cam1");   // armed + start → startCount=1
    REQUIRE(f.ctrl.stateOf("ch1") == RecordingState::Recording);

    // 드롭 엣지: 죽어가는 소스에 start 안 함 — 현재 세그먼트만 종료
    f.ctrl.onReconnect("ch1", "Cam1");
    CHECK(f.sink.stopCount  == 1);
    CHECK(f.sink.startCount == 1);   // 새 start 없음
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Idle);

    // 복구 엣지: armed && Idle → 새 세그먼트 시작(두 번째 세그먼트)
    f.ctrl.onStreaming("ch1", "Cam1");
    CHECK(f.sink.startCount == 2);   // 두 세그먼트(doStart 2회)
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Recording);
}

TEST_CASE("생존: toggle stop 후 armed=false — 이후 onStreaming 무동작") {
    Fixture f;
    f.ctrl.toggle("ch1", "Cam1");   // start
    REQUIRE(f.ctrl.stateOf("ch1") == RecordingState::Recording);

    f.ctrl.toggle("ch1", "Cam1");   // stop → armed=false
    REQUIRE(f.ctrl.stateOf("ch1") == RecordingState::Idle);
    CHECK(f.sink.startCount == 1);

    f.ctrl.onStreaming("ch1", "Cam1");   // 의도 없음 → 무동작
    CHECK(f.sink.startCount == 1);
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Idle);
}

TEST_CASE("생존: 이미 Recording 중 onStreaming은 중복 시작하지 않는다") {
    Fixture f;
    f.ctrl.toggle("ch1", "Cam1");
    REQUIRE(f.ctrl.stateOf("ch1") == RecordingState::Recording);

    f.ctrl.onStreaming("ch1", "Cam1");   // 이미 녹화 중 — 무동작
    CHECK(f.sink.startCount == 1);
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Recording);
}

TEST_CASE("생존: armed가 아닌 채널은 onReconnect/onStreaming 무동작") {
    Fixture f;
    f.ctrl.toggle("ch2", "Cam2");   // ch2만 armed

    f.ctrl.onReconnect("ch1", "Cam1");   // ch1은 비armed — 무동작
    f.ctrl.onStreaming("ch1", "Cam1");   // 비armed — 무동작

    CHECK(f.sink.stopCount  == 0);
    CHECK(f.sink.startCount == 1);   // ch2 start만
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Idle);
}

TEST_CASE("생존: splitOnReconnect=false이면 onReconnect 무동작(녹화 유지)") {
    FakeClock clock;
    FakeRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy noSplit{std::chrono::seconds{600}, false};
    RecordingController ctrl{sink, clock, logger, noSplit};

    ctrl.toggle("ch1", "Cam1");
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);

    ctrl.onReconnect("ch1", "Cam1");   // split 비활성 — 종료 안 함

    CHECK(sink.stopCount == 0);
    CHECK(sink.startCount == 1);   // 최초 start만
    CHECK(ctrl.stateOf("ch1") == RecordingState::Recording);
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

// ── D2: 파일명 충돌 방지(같은 초 stop→start도 다른 경로) ─────────────────────

TEST_CASE("D2: 같은 시각 연속 녹화는 서로 다른 경로를 생성한다(truncate 방지)") {
    Fixture f;
    // FakeClock은 고정 시각이라 두 start의 초 단위 타임스탬프가 동일하다.
    // 단조 시퀀스 접미사 덕분에 경로는 달라야 한다.
    f.ctrl.toggle("ch1", "Cam1");   // start 1
    f.ctrl.toggle("ch1", "Cam1");   // stop
    f.ctrl.toggle("ch1", "Cam1");   // start 2 (같은 시각)

    REQUIRE(f.sink.startCalls.size() == 2);
    CHECK_FALSE(f.sink.startCalls[0].outputPath.empty());
    CHECK_FALSE(f.sink.startCalls[1].outputPath.empty());
    CHECK(f.sink.startCalls[0].outputPath != f.sink.startCalls[1].outputPath);
}

TEST_CASE("D2: 롤오버 두 세그먼트 경로가 서로 다르다(같은 시각이라도)") {
    FakeClock clock;
    FakeRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{60}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    ctrl.toggle("ch1", "Cam1");        // start 1 (시각 T0)
    clock.advance(std::chrono::seconds{60});
    ctrl.tick();                       // 롤오버 → start 2 (시각 T0+60, 초가 같을 수도)

    REQUIRE(sink.startCalls.size() == 2);
    CHECK(sink.startCalls[0].outputPath != sink.startCalls[1].outputPath);
}

// ── D3: REC 표시 수렴(reconciliation) ───────────────────────────────────────

namespace {
// startRecording 성공으로 래치는 올리지만 isRecording은 외부에서 강제로 false로
// 만들 수 있는 페이크 — 디코드 스레드 start 실패(래치만 내려감) 상황을 모사한다.
class FlakyRecordingSink final : public nv::app::IRecordingSink {
public:
    bool startRecording(const std::string& channelId,
                        const std::string& outputPath) override {
        m_recording[channelId] = !forceNotRecording;
        startCalls.push_back({channelId, outputPath});
        ++startCount;
        return true;   // 컨트롤러 관점에선 성공(래치 설정)
    }
    void stopRecording(const std::string& channelId) override {
        m_recording[channelId] = false;
        ++stopCount;
    }
    bool isRecording(const std::string& channelId) const override {
        auto it = m_recording.find(channelId);
        return it != m_recording.end() && it->second;
    }
    bool hasRecordingError(const std::string& /*channelId*/) const override { return false; }
    struct StartCall { std::string channelId; std::string outputPath; };
    bool forceNotRecording = false;   // true면 start 후에도 isRecording==false
    std::vector<StartCall> startCalls;
    int startCount = 0;
    int stopCount  = 0;
private:
    std::unordered_map<std::string, bool> m_recording;
};
} // namespace

TEST_CASE("D3 수렴: Recording인데 sink가 비녹화면 유예 후 tick에서 Idle로 수렴 + 통지") {
    FakeClock clock;
    FlakyRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{600}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    std::vector<std::pair<std::string, RecordingState>> events;
    ctrl.setObserver([&](const std::string& id, RecordingState s) {
        events.push_back({id, s});
    });

    sink.forceNotRecording = true;     // 디코드 스레드 start 실패 모사(래치만 올라감)
    ctrl.toggle("ch1", "Cam1");        // 컨트롤러는 Starting(start 성공 래치, 키프레임 대기)
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Starting);

    // 유예(3초) 전 tick — 아직 수렴하지 않는다(비동기 지연 허용)
    clock.advance(std::chrono::seconds{2});
    ctrl.tick();
    CHECK(ctrl.stateOf("ch1") == RecordingState::Starting);

    // 유예 후 tick — Idle로 수렴
    clock.advance(std::chrono::seconds{2});
    ctrl.tick();
    CHECK(ctrl.stateOf("ch1") == RecordingState::Idle);

    // Idle 통지가 발생해야 한다(UI REC 해제)
    const bool idleNotified = std::any_of(events.begin(), events.end(),
        [](const auto& e){ return e.second == RecordingState::Idle; });
    CHECK(idleNotified);

    // 경고 로그
    const bool hasWarn = std::any_of(logger.entries.begin(), logger.entries.end(),
        [](const auto& e){ return e.level == nv::app::LogLevel::Warn; });
    CHECK(hasWarn);
}

TEST_CASE("생존: reconciliation 발동 후에도 armed 유지 → onStreaming 재시도") {
    FakeClock clock;
    FlakyRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{600}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    sink.forceNotRecording = true;     // 디코드 스레드 start 실패 모사(래치만 올라감)
    ctrl.toggle("ch1", "Cam1");        // armed + Starting(래치, 키프레임 대기)
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Starting);
    REQUIRE(sink.startCount == 1);

    // 유예 후 tick — Idle로 수렴(하지만 armed는 유지)
    clock.advance(std::chrono::seconds{4});
    ctrl.tick();
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Idle);

    // 진짜 복구: 이번엔 정상 녹화 → onStreaming이 재시도해 새 세그먼트 시작
    sink.forceNotRecording = false;
    ctrl.onStreaming("ch1", "Cam1");
    CHECK(sink.startCount == 2);       // armed 유지 덕분에 재시도됨
    CHECK(ctrl.stateOf("ch1") == RecordingState::Recording);
}

TEST_CASE("D3 수렴: sink가 정상 녹화 중이면 tick에서 수렴하지 않는다") {
    FakeClock clock;
    FlakyRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{600}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    ctrl.toggle("ch1", "Cam1");        // 정상 — isRecording==true
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);

    clock.advance(std::chrono::seconds{10});
    ctrl.tick();
    CHECK(ctrl.stateOf("ch1") == RecordingState::Recording);   // 수렴 없음
}

// ── P4d: Starting → Recording 2단계 뱃지 상태 시퀀스 ────────────────────────

namespace {
// startRecording은 항상 성공하지만 isRecording은 외부 플래그로 독립 제어.
// FlakyRecordingSink와 달리 m_recording을 forceRecording 플래그로 직접 오버라이드해
// startRecording 호출 후에도 isRecording==false → true 전환을 자유롭게 제어한다.
class ControllableRecordingSink final : public nv::app::IRecordingSink {
public:
    bool startRecording(const std::string& channelId,
                        const std::string& outputPath) override {
        startCalls.push_back({channelId, outputPath});
        ++startCount;
        return true;   // 항상 성공(래치 설정)
    }
    void stopRecording(const std::string& channelId) override {
        m_actualRecording[channelId] = false;
        ++stopCount;
    }
    bool isRecording(const std::string& channelId) const override {
        auto it = m_actualRecording.find(channelId);
        return it != m_actualRecording.end() && it->second;
    }
    bool hasRecordingError(const std::string& /*channelId*/) const override { return false; }
    // 테스트 헬퍼: 채널의 실제 녹화 상태를 외부에서 직접 설정
    void setRecording(const std::string& channelId, bool value) {
        m_actualRecording[channelId] = value;
    }
    struct StartCall { std::string channelId; std::string outputPath; };
    std::vector<StartCall> startCalls;
    int startCount = 0;
    int stopCount  = 0;
private:
    std::unordered_map<std::string, bool> m_actualRecording;
};
} // namespace (ControllableRecordingSink 정의 — P4d 테스트 전용)

TEST_CASE("P4d: isRecording=false 싱크에서 toggle → Starting 발행, tick 후 isRecording=true → Recording 발행") {
    FakeClock clock;
    ControllableRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{600}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    std::vector<std::pair<std::string, RecordingState>> events;
    ctrl.setObserver([&](const std::string& id, RecordingState s) {
        events.push_back({id, s});
    });

    // sink.startRecording은 성공하지만 isRecording은 false(첫 키프레임 전)
    // sink.setRecording("ch1", false) 가 기본값이므로 그대로 toggle
    ctrl.toggle("ch1", "Cam1");

    // 즉시: Starting 상태 + Starting 통지
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Starting);
    REQUIRE(events.size() == 1);
    CHECK(events[0].second == RecordingState::Starting);

    // sink가 실제로 녹화 시작(첫 키프레임 도달) 모사
    sink.setRecording("ch1", true);
    ctrl.tick();

    // tick 후: Recording 상태 + Recording 통지
    CHECK(ctrl.stateOf("ch1") == RecordingState::Recording);
    REQUIRE(events.size() == 2);
    CHECK(events[1].second == RecordingState::Recording);
}

TEST_CASE("P4d: isRecording=true 싱크(페이크 즉시응답)에서는 toggle 즉시 Recording — Starting 없음") {
    // FakeRecordingSink는 startRecording 즉시 isRecording=true → Recording 바로 전환
    Fixture f;

    std::vector<RecordingState> states;
    f.ctrl.setObserver([&](const std::string& /*id*/, RecordingState s) {
        states.push_back(s);
    });

    f.ctrl.toggle("ch1", "Cam1");

    // 페이크 싱크는 isRecording=true 즉시라 Starting 건너뜀
    REQUIRE(states.size() == 1);
    CHECK(states[0] == RecordingState::Recording);
    CHECK(f.ctrl.stateOf("ch1") == RecordingState::Recording);
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

// ── D10: 디스크/쓰기 오류 가시화 ─────────────────────────────────────────────

TEST_CASE("D10: sink.hasRecordingError=true면 tick이 Idle로 수렴(REC 해제) + 경고") {
    FakeClock clock;
    FakeRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{600}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    std::vector<std::pair<std::string, RecordingState>> events;
    ctrl.setObserver([&](const std::string& id, RecordingState s) {
        events.push_back({id, s});
    });

    ctrl.toggle("ch1", "Cam1");   // 정상 시작 — isRecording==true
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);

    // 디스크 풀 모사: sink는 isRecording==true(m_open 유지)지만 쓰기 오류 발생.
    sink.setRecordingError("ch1", true);

    // 유예(3초)를 넘겨 reconcile/error 검사가 동작하게 한다.
    clock.advance(std::chrono::seconds{4});
    ctrl.tick();

    CHECK(ctrl.stateOf("ch1") == RecordingState::Idle);   // 무음 손실 가시화 → REC 해제

    const bool idleNotified = std::any_of(events.begin(), events.end(),
        [](const auto& e){ return e.second == RecordingState::Idle; });
    CHECK(idleNotified);

    const bool hasWarn = std::any_of(logger.entries.begin(), logger.entries.end(),
        [](const auto& e){ return e.level == nv::app::LogLevel::Warn; });
    CHECK(hasWarn);
}

TEST_CASE("D10: 오류 없으면 정상 녹화는 수렴하지 않는다") {
    FakeClock clock;
    FakeRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{600}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    ctrl.toggle("ch1", "Cam1");
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);

    clock.advance(std::chrono::seconds{10});
    ctrl.tick();
    CHECK(ctrl.stateOf("ch1") == RecordingState::Recording);   // 오류 없음 — 유지
}

// ── D1/D2: 롤오버 재시도 + 백오프 ────────────────────────────────────────────

namespace {
// N회 연속 start를 실패시키고 그 후 성공하는 페이크(롤오버 실패→복구 모사).
class CountedFailRecordingSink final : public nv::app::IRecordingSink {
public:
    bool startRecording(const std::string& channelId,
                        const std::string& /*outputPath*/) override {
        ++startAttempts;
        if (failuresRemaining > 0) { --failuresRemaining; return false; }
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
    bool hasRecordingError(const std::string& /*channelId*/) const override { return false; }

    int failuresRemaining = 0;   // 이 횟수만큼 startRecording이 false
    int startAttempts = 0;       // 시도 총횟수(성공+실패)
    int startCount = 0;          // 성공 횟수
    int stopCount  = 0;
private:
    std::unordered_map<std::string, bool> m_recording;
};
} // namespace

TEST_CASE("D1: 롤오버 doStart 실패 후 다음 tick에서 재시도해 복구한다") {
    FakeClock clock;
    CountedFailRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{60}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    ctrl.toggle("ch1", "Cam1");   // start 1 성공
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);
    REQUIRE(sink.startCount == 1);

    // 롤오버 시점에 1회 실패하도록 설정.
    sink.failuresRemaining = 1;
    clock.advance(std::chrono::seconds{60});
    ctrl.tick();   // 롤오버 → doStart 실패 → armed && Idle (retryStart=true)
    CHECK(ctrl.stateOf("ch1") == RecordingState::Idle);

    // 다음 tick: D1 재시도가 성공해 다시 Recording.
    ctrl.tick();
    CHECK(ctrl.stateOf("ch1") == RecordingState::Recording);
    CHECK(sink.startCount == 2);   // 최초 + 재시도 성공
}

TEST_CASE("D2: doStart 연속 실패가 임계치에 도달하면 armed 해제 + 재시도 중단") {
    FakeClock clock;
    CountedFailRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{60}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    ctrl.toggle("ch1", "Cam1");   // start 1 성공 (startFailures 리셋)
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);

    // 롤오버 후 영구 실패(충분히 큰 실패 수).
    sink.failuresRemaining = 100;
    clock.advance(std::chrono::seconds{60});
    ctrl.tick();   // 롤오버 실패 #1 (retryStart=true)

    // 임계치(5)까지 추가 tick — 각 tick이 D1 재시도(실패)로 카운터 누적.
    for (int i = 0; i < 10; ++i) ctrl.tick();

    // armed가 해제되면 더 이상 재시도하지 않는다 — 시도 횟수가 폭주하지 않음.
    const int attemptsAfterDisarm = sink.startAttempts;
    for (int i = 0; i < 10; ++i) ctrl.tick();
    CHECK(sink.startAttempts == attemptsAfterDisarm);   // 추가 시도 없음(중단됨)
    CHECK(ctrl.stateOf("ch1") == RecordingState::Idle);

    // "반복 실패 — 중단" 경고가 남아야 한다.
    const bool hasStopWarn = std::any_of(logger.entries.begin(), logger.entries.end(),
        [](const auto& e){ return e.level == nv::app::LogLevel::Warn &&
                                   e.message.find("반복 실패") != std::string::npos; });
    CHECK(hasStopWarn);
}

TEST_CASE("D1: onReconnect 드롭 엣지는 tick 재시도를 유발하지 않는다(죽은 소스 보호)") {
    FakeClock clock;
    CountedFailRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{600}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    ctrl.toggle("ch1", "Cam1");
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);

    ctrl.onReconnect("ch1", "Cam1");   // 드롭 — armed && Idle, retryStart=false
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Idle);

    const int attemptsBefore = sink.startAttempts;
    for (int i = 0; i < 5; ++i) ctrl.tick();   // tick은 재시도하지 않아야 함
    CHECK(sink.startAttempts == attemptsBefore);   // 재연결 대기 중 — 시도 없음
    CHECK(ctrl.stateOf("ch1") == RecordingState::Idle);

    // 복구(onStreaming)에서만 재개
    ctrl.onStreaming("ch1", "Cam1");
    CHECK(ctrl.stateOf("ch1") == RecordingState::Recording);
}

// ── D10 백오프: 디스크오류 무한 churn 차단 ───────────────────────────────────

TEST_CASE("D10 백오프: hasRecordingError가 반복되면 임계치에서 armed 해제 + 재시도 중단") {
    FakeClock clock;
    FakeRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{600}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    ctrl.toggle("ch1", "Cam1");        // 정상 시작 — Recording
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);

    sink.setRecordingError("ch1", true);   // 디스크 풀 모사(영구)

    // 충분히 많은 tick: 매 디스크오류 수렴 → ++diskErrors, 사이 tick에서 D1 재시도(doStart 성공 래치).
    // 임계(5) 도달 시 armed 해제 → 더 이상 doStart 재시도하지 않아 startCount가 폭주하지 않는다.
    for (int i = 0; i < 40; ++i) ctrl.tick();

    const int startsAfterDisarm = sink.startCount;
    for (int i = 0; i < 40; ++i) ctrl.tick();
    CHECK(sink.startCount == startsAfterDisarm);   // 중단됨 — 추가 시작 시도 없음(churn 멈춤)
    CHECK(ctrl.stateOf("ch1") == RecordingState::Idle);

    const bool hasStopWarn = std::any_of(logger.entries.begin(), logger.entries.end(),
        [](const auto& e){ return e.level == nv::app::LogLevel::Warn &&
                                  e.message.find("디스크") != std::string::npos &&
                                  e.message.find("반복") != std::string::npos; });
    CHECK(hasStopWarn);
}

TEST_CASE("D10 백오프: 디스크 복구 후 정상 세그먼트가 diskErrors를 리셋한다(거짓 disarm 방지)") {
    FakeClock clock;
    FakeRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{600}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    ctrl.toggle("ch1", "Cam1");
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);

    // 임계 미만(예: 3회)의 디스크오류 수렴을 만든다.
    // tick 패턴: error→Idle, retry→Rec, error→Idle, retry→Rec, error→Idle, retry→Rec, error→Idle
    // 7 ticks → 3회 diskErrors 누적, 마지막 tick은 error수렴이라 Idle로 끝남(임계 5 미만).
    sink.setRecordingError("ch1", true);
    for (int i = 0; i < 7; ++i) ctrl.tick();   // 몇 차례 수렴/재시도 (임계 5 미만 누적되도록)
    REQUIRE(ctrl.stateOf("ch1") != RecordingState::Recording);   // 오류 중 — 미녹화

    // 디스크 복구: 이후 doStart가 정상 녹화로 이어진다.
    sink.setRecordingError("ch1", false);
    ctrl.tick();                               // D1 재시도 → doStart 성공 → Recording
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);
    clock.advance(std::chrono::seconds{31});   // kDiskHealthyResetWindow(30s) 경과
    ctrl.tick();                               // 확인된 정상 세그먼트 → diskErrors 리셋
    CHECK(ctrl.stateOf("ch1") == RecordingState::Recording);

    // 리셋 증명: 다시 디스크오류가 나도 즉시 disarm되지 않고 armed가 유지된 채 재시도가 이어진다.
    sink.setRecordingError("ch1", true);
    const int startsBefore = sink.startCount;
    for (int i = 0; i < 4; ++i) ctrl.tick();   // 임계(5) 미만이라 아직 재시도 지속
    CHECK(sink.startCount > startsBefore);      // 카운터가 리셋됐으므로 재시도가 계속됨(즉시 중단 아님)
}

TEST_CASE("D10 백오프(회귀): 짧은 무오류 윈도우(avio 버퍼)가 있어도 diskErrors가 임계까지 누적돼 disarm된다") {
    FakeClock clock;
    FakeRecordingSink sink;
    FakeLogger logger;
    SegmentPolicy policy{std::chrono::seconds{600}, true};
    RecordingController ctrl{sink, clock, logger, policy};

    ctrl.toggle("ch1", "Cam1");
    REQUIRE(ctrl.stateOf("ch1") == RecordingState::Recording);

    // 실디스크풀 사이클 모사: 오류 표면화 → 수렴 → 재시도 doStart → (avio 버퍼 덕에) 잠깐
    // isRecording==true·무오류 윈도우(윈도우 30s 미만이라 리셋 금지여야 함) → 다시 오류.
    // 윈도우가 30s 미만이면 diskErrors가 리셋되지 않고 누적돼 임계(5)에서 disarm되어야 한다.
    for (int cycle = 0; cycle < 12; ++cycle) {
        sink.setRecordingError("ch1", true);     // 버퍼 플러시 시 ENOSPC 표면화
        clock.advance(std::chrono::seconds{1});
        ctrl.tick();                              // hasRecordingError → 수렴(++diskErrors), stop
        sink.setRecordingError("ch1", false);     // 다음 세그먼트는 잠깐 무오류(버퍼 윈도우)
        ctrl.tick();                              // D1 재시도 → doStart → Recording(isRecording=true)
        clock.advance(std::chrono::seconds{5});   // 5s < 30s 윈도우 — 리셋되면 안 됨
        ctrl.tick();                              // 짧은 무오류 윈도우 tick (거짓 리셋 시도 지점)
    }

    // 임계 누적으로 disarm되어 churn이 멈춰야 한다: 더 돌려도 새 시작이 늘지 않는다.
    const int startsAfter = sink.startCount;
    sink.setRecordingError("ch1", true);
    for (int i = 0; i < 10; ++i) ctrl.tick();
    CHECK(sink.startCount == startsAfter);          // churn 중단(추가 시작 없음)
    CHECK(ctrl.stateOf("ch1") == RecordingState::Idle);

    const bool disarmWarn = std::any_of(logger.entries.begin(), logger.entries.end(),
        [](const auto& e){ return e.level == nv::app::LogLevel::Warn &&
                                  e.message.find("디스크") != std::string::npos &&
                                  e.message.find("반복") != std::string::npos; });
    CHECK(disarmWarn);
}
