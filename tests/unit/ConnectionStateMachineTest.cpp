#include <catch2/catch_test_macros.hpp>
#include "src/domain/connection/ConnectionStateMachine.h"

using namespace nv::domain;
using namespace std::chrono_literals;

namespace {
ConnectionStateMachine makeSm() {
    return ConnectionStateMachine{ReconnectPolicy{}, StallPolicy{}};
}
// 테스트 전용 기준 시각. steady_clock을 호출하지 않고 임의 epoch에서 시작한다.
constexpr ConnectionStateMachine::TimePoint t0{};

// 가짜연결 사이클을 반복해 Failed까지 도달시키고 도달 시각을 반환하는 헬퍼.
inline ConnectionStateMachine::TimePoint driveToFailed(ConnectionStateMachine& sm) {
    sm.connectRequested(t0);
    auto now = t0;
    while (sm.state() != ConnState::Failed) {
        now += 100ms; sm.sessionOpened(now);
        now += 6s;    sm.tick(now);
        if (sm.state() == ConnState::Failed) break;
        now += 6s;    sm.tick(now);
    }
    return now;
}
}

TEST_CASE("초기 상태는 Idle, 액션 없음") {
    auto sm = makeSm();
    CHECK(sm.state() == ConnState::Idle);
    CHECK(sm.reconnectAttempts() == 0);
}

TEST_CASE("connectRequested: Idle → Connecting, OpenSource 지시") {
    auto sm = makeSm();
    auto t = sm.connectRequested(t0);
    CHECK(sm.state() == ConnState::Connecting);
    CHECK(t.action == Action::OpenSource);
}

TEST_CASE("sessionOpened: Connecting → SessionOpen (아직 연결 확정 아님)") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    auto t = sm.sessionOpened(t0 + 100ms);
    CHECK(sm.state() == ConnState::SessionOpen);
    CHECK(t.action == Action::None);
}

TEST_CASE("packetReceived: SessionOpen → Streaming") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    sm.sessionOpened(t0 + 100ms);
    sm.packetReceived(t0 + 200ms);
    CHECK(sm.state() == ConnState::Streaming);
}

TEST_CASE("framePresented가 와야 비로소 재시도 카운터가 리셋된다 (D2)") {
    auto sm = makeSm();
    // 한 번 실패해서 attempts를 1로 만든 뒤
    sm.connectRequested(t0);
    sm.errorOccurred(DiagnosisReason::SessionRefused, t0 + 1s);
    CHECK(sm.state() == ConnState::Reconnecting);
    CHECK(sm.reconnectAttempts() == 1);
    // 재시도 성공 경로
    sm.tick(t0 + 7s);                       // retryDelay(5s) 경과 → Connecting
    sm.sessionOpened(t0 + 7s + 100ms);
    sm.packetReceived(t0 + 7s + 200ms);
    CHECK(sm.reconnectAttempts() == 1);     // 패킷만으로는 리셋 금지
    sm.framePresented(t0 + 7s + 300ms);
    CHECK(sm.reconnectAttempts() == 0);     // 표시 확정 시에만 리셋
}

TEST_CASE("disconnectRequested: 어느 상태에서든 Idle + CloseSource, 카운터 리셋") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    sm.errorOccurred(DiagnosisReason::SessionRefused, t0 + 1s);
    auto t = sm.disconnectRequested(t0 + 2s);
    CHECK(sm.state() == ConnState::Idle);
    CHECK(t.action == Action::CloseSource);
    CHECK(sm.reconnectAttempts() == 0);
}

TEST_CASE("회귀: 가짜 연결 — SessionOpen 5초 내 패킷 없으면 카운터 유지한 채 재접속") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    sm.sessionOpened(t0 + 100ms);
    CHECK(sm.state() == ConnState::SessionOpen);

    // 4.9초: 아직 유예 내 — 아무 일 없음
    sm.tick(t0 + 100ms + 4900ms);
    CHECK(sm.state() == ConnState::SessionOpen);

    // 5.1초: 가짜 연결 판정
    auto t = sm.tick(t0 + 100ms + 5100ms);
    CHECK(sm.state() == ConnState::Reconnecting);
    CHECK(t.action == Action::CloseSource);
    CHECK(sm.reconnectAttempts() == 1);
    CHECK(sm.lastReason() == DiagnosisReason::NoPackets);
}

TEST_CASE("회귀: 가짜 연결이 반복되어도 카운터는 누적된다 (기존 무한루프 버그)") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    auto now = t0;
    // 가짜 연결 사이클 3회: open → sessionOpened → 5초 침묵 → 재접속
    for (int i = 1; i <= 3; ++i) {
        now += 100ms;
        sm.sessionOpened(now);
        now += 6s;
        sm.tick(now);                        // 가짜 판정 → Reconnecting
        CHECK(sm.reconnectAttempts() == i);  // 절대 0으로 돌아가지 않는다
        now += 6s;
        sm.tick(now);                        // retryDelay 경과 → Connecting
        CHECK(sm.state() == ConnState::Connecting);
    }
}

TEST_CASE("회귀: 가짜 연결 30회 초과 시 Failed로 수렴한다 (고속 churn 차단 보장)") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    auto now = t0;
    for (int i = 0; i < 31; ++i) {
        now += 100ms;
        sm.sessionOpened(now);
        now += 6s;
        sm.tick(now);
        if (sm.state() == ConnState::Failed) break;
        now += 6s;
        sm.tick(now);
        REQUIRE(sm.state() == ConnState::Connecting);
    }
    CHECK(sm.state() == ConnState::Failed);
    CHECK(sm.lastReason() == DiagnosisReason::GaveUp);
}

TEST_CASE("D1: Failed에서 고속 재시도는 멈춘다 — retryDelay 간격 tick으로는 재시도 없음") {
    auto sm = makeSm();
    auto now = driveToFailed(sm);
    sm.tick(now + 6s);                        // 고속 간격(5s)보다 긴 6초여도
    CHECK(sm.state() == ConnState::Failed);   // 저빈도 모드라 아직 재시도 안 함
    CHECK(sm.lastReason() == DiagnosisReason::GaveUp);
}

TEST_CASE("D1: Failed는 저빈도 재시도 모드 — slowRetryDelay(60s) 경과 시 재접속") {
    auto sm = makeSm();
    auto now = driveToFailed(sm);
    auto t = sm.tick(now + 61s);
    CHECK(sm.state() == ConnState::Connecting);
    CHECK(t.action == Action::OpenSource);
    // 저빈도 재시도도 실패하면 다시 Failed + 다음 60초 대기로 돌아간다
    sm.errorOccurred(DiagnosisReason::SessionRefused, now + 62s);
    CHECK(sm.state() == ConnState::Failed);
    sm.tick(now + 62s + 6s);
    CHECK(sm.state() == ConnState::Failed);   // 여전히 저빈도 간격 유지
}

TEST_CASE("D1: Failed에서 sourceAvailableHint → 즉시 재접속 (relay 헬스 부활 경로)") {
    auto sm = makeSm();
    auto now = driveToFailed(sm);
    auto t = sm.sourceAvailableHint(now + 1s);  // 60초 안 기다리고
    CHECK(sm.state() == ConnState::Connecting);
    CHECK(t.action == Action::OpenSource);
}

TEST_CASE("sourceAvailableHint는 Failed 외 상태에선 무시된다") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    auto t = sm.sourceAvailableHint(t0 + 1s);
    CHECK(sm.state() == ConnState::Connecting);  // 변화 없음
    CHECK(t.action == Action::None);
}

TEST_CASE("Failed에서 retryRequested는 카운터를 리셋하며 새 사이클을 연다") {
    auto sm = makeSm();
    auto now = driveToFailed(sm);
    auto t = sm.retryRequested(now + 1s);
    CHECK(sm.state() == ConnState::Connecting);
    CHECK(t.action == Action::OpenSource);
    CHECK(sm.reconnectAttempts() == 0);   // 사용자 명령 = 새 사이클 (sourceAvailableHint는 리셋 안 함)
}

TEST_CASE("Streaming 중 패킷 공백 10초 → Stalled + CloseSource") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    sm.sessionOpened(t0 + 100ms);
    sm.packetReceived(t0 + 200ms);
    sm.framePresented(t0 + 300ms);
    CHECK(sm.state() == ConnState::Streaming);

    sm.tick(t0 + 200ms + 9900ms);            // 9.9초 공백: 유지
    CHECK(sm.state() == ConnState::Streaming);

    auto t = sm.tick(t0 + 200ms + 10100ms);  // 10.1초 공백: stall
    CHECK(sm.state() == ConnState::Stalled);
    CHECK(t.action == Action::CloseSource);
    CHECK(sm.reconnectAttempts() == 1);
}

TEST_CASE("패킷이 계속 오면 stall은 발동하지 않는다") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    sm.sessionOpened(t0 + 100ms);
    auto now = t0 + 200ms;
    sm.packetReceived(now);
    for (int i = 0; i < 60; ++i) {           // 1분간 1초 간격 패킷 + tick
        now += 1s;
        sm.packetReceived(now);
        sm.tick(now);
        REQUIRE(sm.state() == ConnState::Streaming);
    }
}

TEST_CASE("Stalled에서 retryDelay 경과 → Connecting + OpenSource") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    sm.sessionOpened(t0 + 100ms);
    sm.packetReceived(t0 + 200ms);
    sm.tick(t0 + 200ms + 11s);               // stall → Stalled (retryAt = +5s)
    REQUIRE(sm.state() == ConnState::Stalled);
    auto t = sm.tick(t0 + 200ms + 11s + 5100ms);
    CHECK(sm.state() == ConnState::Connecting);
    CHECK(t.action == Action::OpenSource);
}

TEST_CASE("어댑터 에러: Streaming 중 errorOccurred → Stalled 경로") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    sm.sessionOpened(t0 + 100ms);
    sm.packetReceived(t0 + 200ms);
    auto t = sm.errorOccurred(DiagnosisReason::DecodeError, t0 + 1s);
    CHECK(sm.state() == ConnState::Stalled);
    CHECK(t.action == Action::CloseSource);
    CHECK(sm.lastReason() == DiagnosisReason::DecodeError);
}

TEST_CASE("재시도 대기 중 늦게 도착한 어댑터 에러는 무시된다") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    sm.errorOccurred(DiagnosisReason::SessionRefused, t0 + 1s);
    REQUIRE(sm.state() == ConnState::Reconnecting);
    sm.errorOccurred(DiagnosisReason::DeviceUnreachable, t0 + 2s);  // 늦은 이벤트
    CHECK(sm.state() == ConnState::Reconnecting);
    CHECK(sm.reconnectAttempts() == 1);       // 이중 카운트 금지
    CHECK(sm.lastReason() == DiagnosisReason::SessionRefused);
}

TEST_CASE("Idle에서 어댑터 이벤트는 모두 무시된다") {
    auto sm = makeSm();
    sm.sessionOpened(t0);
    sm.packetReceived(t0);
    sm.framePresented(t0);
    sm.errorOccurred(DiagnosisReason::NoPackets, t0);
    sm.tick(t0 + 1h);
    CHECK(sm.state() == ConnState::Idle);
    CHECK(sm.reconnectAttempts() == 0);
}

TEST_CASE("오프스크린 복원력: 표시 없이도 지속 스트리밍이면 패킷만으로 카운터가 리셋된다") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    sm.errorOccurred(DiagnosisReason::SessionRefused, t0 + 1s);  // attempts=1
    REQUIRE(sm.reconnectAttempts() == 1);

    sm.tick(t0 + 7s);                       // retryDelay(5s) 경과 → Connecting
    sm.sessionOpened(t0 + 7s + 100ms);
    auto now = t0 + 7s + 200ms;
    sm.packetReceived(now);                 // → Streaming, streamingSince=now
    REQUIRE(sm.state() == ConnState::Streaming);
    CHECK(sm.reconnectAttempts() == 1);     // 아직 안정 윈도우(3s) 미경과 — 리셋 안 됨

    // 프레임 표시(framePresented) 전혀 없이(오프스크린 모사) 지속 패킷만 공급
    for (int i = 0; i < 5; ++i) { now += 1s; sm.packetReceived(now); }
    CHECK(sm.reconnectAttempts() == 0);     // 지속 스트리밍 확인 → 표시 없이도 리셋
}

TEST_CASE("플래핑 보호: 안정 윈도우 미만의 짧은 스트리밍은 카운터를 리셋하지 않는다") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    sm.errorOccurred(DiagnosisReason::SessionRefused, t0 + 1s);  // attempts=1
    REQUIRE(sm.reconnectAttempts() == 1);

    sm.tick(t0 + 7s);
    sm.sessionOpened(t0 + 7s + 100ms);
    auto now = t0 + 7s + 200ms;
    sm.packetReceived(now);                 // Streaming 진입
    now += 1s; sm.packetReceived(now);      // 윈도우(3s) 미만 — 아직 리셋 금지
    CHECK(sm.reconnectAttempts() == 1);

    // 곧 stall로 빠지면 카운터는 누적된다(리셋되지 않았으므로 2가 됨)
    sm.tick(now + 10100ms);
    CHECK(sm.state() == ConnState::Stalled);
    CHECK(sm.reconnectAttempts() == 2);     // 리셋 안 됐으니 1→2 누적
}

TEST_CASE("재진입: stall 후 다시 스트리밍하면 안정 윈도우가 새로 시작된다") {
    auto sm = makeSm();
    sm.connectRequested(t0);
    sm.sessionOpened(t0 + 100ms);
    auto now = t0 + 200ms;
    sm.packetReceived(now);                 // Streaming
    now += 10100ms; sm.tick(now);           // stall → Stalled, attempts=1, streamingSince clear
    REQUIRE(sm.state() == ConnState::Stalled);
    REQUIRE(sm.reconnectAttempts() == 1);

    now += 5100ms; sm.tick(now);            // retryDelay → Connecting
    sm.sessionOpened(now + 100ms);
    now += 200ms; sm.packetReceived(now);   // 재진입 → 새 streamingSince
    CHECK(sm.reconnectAttempts() == 1);     // 재진입 직후 — 윈도우 리셋, 아직 0 아님
    for (int i = 0; i < 5; ++i) { now += 1s; sm.packetReceived(now); }
    CHECK(sm.reconnectAttempts() == 0);     // 새 윈도우 충족 → 리셋
}
