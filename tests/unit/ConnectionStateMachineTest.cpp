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
