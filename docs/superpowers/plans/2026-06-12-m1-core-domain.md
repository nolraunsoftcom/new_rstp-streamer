# M1-코어: 도메인 상태머신 + 앱 오케스트레이션 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** FFmpeg/Qt 의존 없이 전부 단위테스트 가능한 new_viewer의 심장 — 연결 상태머신, 진단 모델, 채널 오케스트레이션 — 을 TDD로 구축한다.

**Architecture:** 클린아키텍처의 Domain(순수 C++20, 의존 0)과 Application(포트 인터페이스 + ChannelController) 레이어만 구현. 시간은 전부 주입(TimePoint 파라미터)이라 fake clock으로 모든 시간 기반 전이를 ms 단위에 검증한다. 설계 문서: `docs/2026-06-12-new-viewer-design.md` (특히 D1, D2, D5).

**Tech Stack:** C++20, CMake ≥3.24, Catch2 v3 (FetchContent), 외부 의존 없음 (FFmpeg/Qt는 다음 플랜 M1-파이프라인에서).

**2026-06-12 설계 3차 리뷰 반영:** ① D1 변경 — `Failed`는 영구 정지가 아니라 **저빈도 재시도 모드**(기본 60초 간격, relay 헬스 신호로 즉시 부활). 장비가 휴대 카메라라 전원 차단·배터리 교체가 정상 흐름이기 때문. ② 상태머신/ChannelController의 사용 스레드는 UI 스레드가 아니라 **전용 control 스레드** (단일 스레드 보장 원칙은 동일).

**범위 제외 (M1-파이프라인 플랜으로):** FfmpegStreamSource, UI, 통합테스트(MediaMTX+ffmpeg publish), 실장비 24h 소크, frameDecoded 단계의 실제 발생원.

---

## 설계 요점 (구현자가 알아야 할 도메인 규칙)

상태: `Idle → Connecting → SessionOpen → Streaming` / 장애 시 `Stalled`(스트리밍 중 데이터 두절) 또는 `Reconnecting`(그 외 사유) → 재시도 → 한도 초과 시 `Failed`.

철칙 (기존 viewer의 버그에서 격상된 규칙):
1. **세션 열림(SDP 수신)은 연결 확정이 아니다.** `SessionOpen`에서 `dataConfirmTimeout`(5s) 내 패킷이 없으면 가짜 연결 → 재시도 카운터 **유지한 채** 재접속.
2. **재시도 카운터 리셋은 `framePresented`(프레임 표시 확정)에서만.** 단, 사용자 명령(`connectRequested`/`retryRequested`/`disconnectRequested`)은 새 사이클 시작이므로 리셋한다 — D2의 취지는 "자동 루프 내 리셋 금지"다.
3. **`Failed`는 고속 재시도의 끝이지 영구 정지가 아니다 (D1, 3차 리뷰).** `Failed` 진입 시 고속 재시도(`retryDelay` 5s)는 멈추고 **저빈도 재시도(`slowRetryDelay` 60s)**로 전환한다. 빠져나오는 경로는 셋: ① 60초 경과 후 `tick`의 저빈도 재시도, ② `sourceAvailableHint`(relay 헬스체크가 소스 복귀 감지 — M4에서 배선, 상태머신 입력은 지금 정의), ③ 수동 `retryRequested`(카운터 리셋). 기존 give-up의 목적(가짜 연결 고속 churn 차단)은 고속 재시도 중지만으로 달성된다.
4. 상태머신은 시간을 조회하지 않는다 — 모든 입력이 `TimePoint now`를 받는다.
5. 연결 시도 자체의 타임아웃은 어댑터(FFmpeg) 책임 — `errorOccurred`로 들어온다. 상태머신은 `Connecting`에 자체 데드라인을 두지 않는다.

## 파일 구조 (이 플랜이 만드는 것)

```
new_viewer/
  CMakeLists.txt                                  # 루트: C++20, src+tests
  .gitignore
  src/
    CMakeLists.txt                                # nv_domain, nv_app 라이브러리
    domain/
      health/DiagnosisReason.h                    # 원인 코드 enum + 문자열 변환
      health/StreamHealth.h                       # 진단 6단계 모델
      connection/Policies.h                       # ReconnectPolicy, StallPolicy
      connection/ConnectionStateMachine.h/.cpp    # 핵심 상태머신
    app/
      ports/IClock.h  ports/ILogger.h  ports/IStreamSource.h
      ChannelController.h/.cpp                    # 상태머신 ↔ 포트 오케스트레이션
  tests/
    CMakeLists.txt                                # Catch2 FetchContent
    unit/SmokeTest.cpp
    unit/ConnectionStateMachineTest.cpp
    unit/ChannelControllerTest.cpp
    helpers/FakeClock.h  helpers/FakeStreamSource.h  helpers/FakeLogger.h
```

---

### Task 1: 프로젝트 골격 (빌드 + 테스트 러너)

**Files:**
- Create: `CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/unit/SmokeTest.cpp`, `.gitignore`

- [ ] **Step 1: 루트 CMakeLists.txt 작성**

```cmake
cmake_minimum_required(VERSION 3.24)
project(new_viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
```

- [ ] **Step 2: src/CMakeLists.txt 작성 (라이브러리 2개, 아직 소스 없음 → INTERFACE로 시작하지 않고 빈 placeholder 없이 Task 2에서 소스와 함께 STATIC 전환)**

Task 1 시점에는 src에 컴파일할 소스가 없으므로 일단 빈 파일로 두지 말고 다음 내용만:

```cmake
# 라이브러리는 Task 2부터 소스가 생기면 추가된다.
```

- [ ] **Step 3: tests/CMakeLists.txt 작성 (Catch2 v3 FetchContent + 스모크 테스트)**

```cmake
include(FetchContent)
FetchContent_Declare(
  catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.4
)
FetchContent_MakeAvailable(catch2)

add_executable(nv_unit_tests
  unit/SmokeTest.cpp
)
target_include_directories(nv_unit_tests PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nv_unit_tests PRIVATE Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(nv_unit_tests)
```

- [ ] **Step 4: tests/unit/SmokeTest.cpp 작성**

```cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("smoke: test runner works") {
    CHECK(1 + 1 == 2);
}
```

- [ ] **Step 5: .gitignore 작성**

```
build/
.DS_Store
compile_commands.json
.cache/
.omc/
```

- [ ] **Step 6: 빌드 + 테스트 실행으로 골격 검증**

Run: `cmake -B build -S . && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 1`

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/CMakeLists.txt tests/ .gitignore
git commit -m "build: 프로젝트 골격 — CMake + Catch2 테스트 러너"
```

---

### Task 2: 진단 모델 — DiagnosisReason, StreamHealth, 정책 값 객체

**Files:**
- Create: `src/domain/health/DiagnosisReason.h`, `src/domain/health/StreamHealth.h`, `src/domain/connection/Policies.h`
- Modify: `src/CMakeLists.txt`
- Test: `tests/unit/StreamHealthTest.cpp`

- [ ] **Step 1: 실패하는 테스트 작성 — tests/unit/StreamHealthTest.cpp**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "src/domain/health/DiagnosisReason.h"
#include "src/domain/health/StreamHealth.h"

using namespace nv::domain;

TEST_CASE("DiagnosisReason: 모든 코드에 사람이 읽을 라벨이 있다") {
    CHECK(toString(DiagnosisReason::None) == "None");
    CHECK(toString(DiagnosisReason::DeviceUnreachable) == "DeviceUnreachable");
    CHECK(toString(DiagnosisReason::RelayDown) == "RelayDown");
    CHECK(toString(DiagnosisReason::RelayNoSource) == "RelayNoSource");
    CHECK(toString(DiagnosisReason::SessionRefused) == "SessionRefused");
    CHECK(toString(DiagnosisReason::NoPackets) == "NoPackets");
    CHECK(toString(DiagnosisReason::DecodeError) == "DecodeError");
    CHECK(toString(DiagnosisReason::DiskLow) == "DiskLow");
    CHECK(toString(DiagnosisReason::DiskFull) == "DiskFull");
    CHECK(toString(DiagnosisReason::GaveUp) == "GaveUp");
}

TEST_CASE("StreamHealth: 초기 상태는 모든 단계 Unknown") {
    StreamHealth h;
    for (auto stage : kAllHealthStages) {
        CHECK(h.stageState(stage) == StageState::Unknown);
    }
    CHECK(h.failedReason() == DiagnosisReason::None);
}

TEST_CASE("StreamHealth: 단계 도달을 기록하면 그 단계까지 Ok") {
    StreamHealth h;
    h.markReached(HealthStage::PacketFlow);
    CHECK(h.stageState(HealthStage::RtspSession) == StageState::Ok);  // 이전 단계도 Ok
    CHECK(h.stageState(HealthStage::PacketFlow) == StageState::Ok);
    CHECK(h.stageState(HealthStage::Decoding) == StageState::Unknown);
}

TEST_CASE("StreamHealth: 실패를 기록하면 해당 단계 Failed + 원인 보존") {
    StreamHealth h;
    h.markReached(HealthStage::RtspSession);
    h.markFailed(HealthStage::PacketFlow, DiagnosisReason::NoPackets);
    CHECK(h.stageState(HealthStage::PacketFlow) == StageState::Failed);
    CHECK(h.failedReason() == DiagnosisReason::NoPackets);
    CHECK(h.stageState(HealthStage::RtspSession) == StageState::Ok); // 도달한 단계는 유지
}

TEST_CASE("StreamHealth: reset은 전 단계 Unknown으로 (재접속 사이클 시작)") {
    StreamHealth h;
    h.markReached(HealthStage::Presenting);
    h.reset();
    CHECK(h.stageState(HealthStage::Presenting) == StageState::Unknown);
    CHECK(h.failedReason() == DiagnosisReason::None);
}

TEST_CASE("RelayIntake 단계는 직결 모드에서 NotApplicable로 둘 수 있다") {
    StreamHealth h;
    h.markNotApplicable(HealthStage::RelayIntake);
    h.markReached(HealthStage::PacketFlow);
    CHECK(h.stageState(HealthStage::RelayIntake) == StageState::NotApplicable);
}
```

- [ ] **Step 2: 테스트가 컴파일 실패하는지 확인**

tests/CMakeLists.txt의 `add_executable`에 `unit/StreamHealthTest.cpp` 추가 후:
Run: `cmake --build build -j`
Expected: FAIL — `DiagnosisReason.h: No such file or directory`

- [ ] **Step 3: src/domain/health/DiagnosisReason.h 구현**

```cpp
#pragma once
#include <string_view>

namespace nv::domain {

// 설계 D5: UI 문구·로그·테스트가 공유하는 단일 원인 코드.
enum class DiagnosisReason {
    None,
    DeviceUnreachable,
    RelayDown,
    RelayNoSource,
    SessionRefused,
    NoPackets,
    DecodeError,
    DiskLow,
    DiskFull,
    GaveUp,
};

constexpr std::string_view toString(DiagnosisReason r) {
    switch (r) {
        case DiagnosisReason::None:              return "None";
        case DiagnosisReason::DeviceUnreachable: return "DeviceUnreachable";
        case DiagnosisReason::RelayDown:         return "RelayDown";
        case DiagnosisReason::RelayNoSource:     return "RelayNoSource";
        case DiagnosisReason::SessionRefused:    return "SessionRefused";
        case DiagnosisReason::NoPackets:         return "NoPackets";
        case DiagnosisReason::DecodeError:       return "DecodeError";
        case DiagnosisReason::DiskLow:           return "DiskLow";
        case DiagnosisReason::DiskFull:          return "DiskFull";
        case DiagnosisReason::GaveUp:            return "GaveUp";
    }
    return "Unknown";
}

} // namespace nv::domain
```

- [ ] **Step 4: src/domain/health/StreamHealth.h 구현**

```cpp
#pragma once
#include <array>
#include <cstddef>
#include "DiagnosisReason.h"

namespace nv::domain {

// 진단 6단계 (설계 §2 신규 기능). 순서가 곧 파이프라인 순서다.
enum class HealthStage : std::size_t {
    DeviceReach = 0,  // 장비 도달
    RelayIntake = 1,  // 장비→Relay 수신 (직결 모드에서는 NotApplicable)
    RtspSession = 2,  // RTSP 세션 열림
    PacketFlow  = 3,  // 패킷 수신 중
    Decoding    = 4,  // 디코딩 성공
    Presenting  = 5,  // 프레임 표시
};

inline constexpr std::array kAllHealthStages = {
    HealthStage::DeviceReach, HealthStage::RelayIntake, HealthStage::RtspSession,
    HealthStage::PacketFlow,  HealthStage::Decoding,    HealthStage::Presenting,
};

enum class StageState { Unknown, Ok, Failed, NotApplicable };

class StreamHealth {
public:
    // stage까지(이전 단계 포함) 도달을 기록. NotApplicable 단계는 건너뛴다.
    void markReached(HealthStage stage) {
        const auto idx = static_cast<std::size_t>(stage);
        for (std::size_t i = 0; i <= idx; ++i) {
            if (m_states[i] != StageState::NotApplicable) m_states[i] = StageState::Ok;
        }
    }

    void markFailed(HealthStage stage, DiagnosisReason reason) {
        m_states[static_cast<std::size_t>(stage)] = StageState::Failed;
        m_reason = reason;
    }

    void markNotApplicable(HealthStage stage) {
        m_states[static_cast<std::size_t>(stage)] = StageState::NotApplicable;
    }

    void reset() {
        for (auto& s : m_states) {
            if (s != StageState::NotApplicable) s = StageState::Unknown;
        }
        m_reason = DiagnosisReason::None;
        // NotApplicable은 모드 속성이므로 reset에도 유지된다.
    }

    StageState stageState(HealthStage stage) const {
        return m_states[static_cast<std::size_t>(stage)];
    }

    DiagnosisReason failedReason() const { return m_reason; }

private:
    std::array<StageState, kAllHealthStages.size()> m_states{};  // 모두 Unknown(0)
    DiagnosisReason m_reason = DiagnosisReason::None;
};

} // namespace nv::domain
```

주의: `reset()` 후 `failedReason() == None`이어야 하므로 `StageState`의 첫 enumerator가 `Unknown`이어야 한다(value-init이 Unknown이 되도록). 위 코드가 그렇게 되어 있다.

- [ ] **Step 5: src/domain/connection/Policies.h 구현**

```cpp
#pragma once
#include <chrono>

namespace nv::domain {

struct ReconnectPolicy {
    int maxAttempts = 30;                              // 초과 시 Failed (저빈도 모드 전환)
    std::chrono::milliseconds retryDelay{5000};        // 고속 재시도 간격
    std::chrono::milliseconds slowRetryDelay{60000};   // Failed에서의 저빈도 재시도 간격 (D1)
};

struct StallPolicy {
    std::chrono::milliseconds dataConfirmTimeout{5000}; // SessionOpen에서 첫 패킷 대기 한도 (가짜연결 판정)
    std::chrono::milliseconds stallTimeout{10000};      // Streaming에서 패킷 공백 한도
};

} // namespace nv::domain
```

- [ ] **Step 6: src/CMakeLists.txt를 라이브러리 정의로 교체**

```cmake
add_library(nv_domain INTERFACE)
target_include_directories(nv_domain INTERFACE ${CMAKE_SOURCE_DIR})
# ConnectionStateMachine.cpp가 생기는 Task 3에서 STATIC으로 전환한다.
```

tests/CMakeLists.txt의 링크에 `nv_domain` 추가:

```cmake
target_link_libraries(nv_unit_tests PRIVATE nv_domain Catch2::Catch2WithMain)
```

- [ ] **Step 7: 테스트 통과 확인**

Run: `cmake -B build -S . && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 전체 PASS (스모크 1 + 신규 6 케이스)

- [ ] **Step 8: Commit**

```bash
git add src/domain src/CMakeLists.txt tests/
git commit -m "feat(domain): 진단 모델 — DiagnosisReason, StreamHealth 6단계, 정책 값 객체"
```

---

### Task 3: ConnectionStateMachine — 정상 경로

**Files:**
- Create: `src/domain/connection/ConnectionStateMachine.h`, `src/domain/connection/ConnectionStateMachine.cpp`
- Modify: `src/CMakeLists.txt` (STATIC 전환)
- Test: `tests/unit/ConnectionStateMachineTest.cpp`

- [ ] **Step 1: 실패하는 테스트 작성 — 정상 경로 전이**

```cpp
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
```

tests/CMakeLists.txt의 `add_executable`에 `unit/ConnectionStateMachineTest.cpp` 추가.

- [ ] **Step 2: 컴파일 실패 확인**

Run: `cmake --build build -j`
Expected: FAIL — `ConnectionStateMachine.h: No such file`

- [ ] **Step 3: ConnectionStateMachine.h 구현**

```cpp
#pragma once
#include <chrono>
#include <optional>
#include "Policies.h"
#include "src/domain/health/DiagnosisReason.h"

namespace nv::domain {

enum class ConnState { Idle, Connecting, SessionOpen, Streaming, Stalled, Reconnecting, Failed };

constexpr std::string_view toString(ConnState s) {
    switch (s) {
        case ConnState::Idle:         return "Idle";
        case ConnState::Connecting:   return "Connecting";
        case ConnState::SessionOpen:  return "SessionOpen";
        case ConnState::Streaming:    return "Streaming";
        case ConnState::Stalled:      return "Stalled";
        case ConnState::Reconnecting: return "Reconnecting";
        case ConnState::Failed:       return "Failed";
    }
    return "Unknown";
}

// 상태머신이 호출자(앱 계층)에 지시하는 부수효과. 상태머신 자신은 아무것도 실행하지 않는다.
enum class Action { None, OpenSource, CloseSource };

struct Transition {
    Action action = Action::None;
};

// 설계 D1/D2. 시간은 절대 조회하지 않는다 — 모든 입력이 now를 받는다.
class ConnectionStateMachine {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    ConnectionStateMachine(ReconnectPolicy reconnect, StallPolicy stall);

    // 사용자 명령 (새 사이클 — 카운터 리셋 허용)
    Transition connectRequested(TimePoint now);
    Transition disconnectRequested(TimePoint now);
    Transition retryRequested(TimePoint now);   // Failed에서만 의미. 그 외엔 무시.

    // 외부 신호: relay 헬스체크가 소스 복귀를 감지 (M4에서 배선).
    // Failed에서만 즉시 재접속을 트리거하고, 다른 상태에선 무시한다 (D1).
    Transition sourceAvailableHint(TimePoint now);

    // 어댑터 이벤트
    Transition sessionOpened(TimePoint now);
    Transition packetReceived(TimePoint now);
    Transition framePresented(TimePoint now);
    Transition errorOccurred(DiagnosisReason reason, TimePoint now);

    // 주기 입력 (1초 주기 권장)
    Transition tick(TimePoint now);

    ConnState state() const { return m_state; }
    int reconnectAttempts() const { return m_attempts; }
    DiagnosisReason lastReason() const { return m_lastReason; }

private:
    Transition beginRetryOrFail(DiagnosisReason reason, ConnState retryState, TimePoint now);

    ReconnectPolicy m_reconnect;
    StallPolicy m_stall;
    ConnState m_state = ConnState::Idle;
    int m_attempts = 0;
    DiagnosisReason m_lastReason = DiagnosisReason::None;
    std::optional<TimePoint> m_dataDeadline;   // SessionOpen: 첫 패킷 데드라인
    std::optional<TimePoint> m_lastPacketAt;   // Streaming: stall 감지용
    std::optional<TimePoint> m_retryAt;        // Reconnecting/Stalled: 재시도 시각
};

} // namespace nv::domain
```

- [ ] **Step 4: ConnectionStateMachine.cpp 구현 (정상 경로 + 공통 재시도 헬퍼)**

```cpp
#include "ConnectionStateMachine.h"

namespace nv::domain {

ConnectionStateMachine::ConnectionStateMachine(ReconnectPolicy reconnect, StallPolicy stall)
    : m_reconnect(reconnect), m_stall(stall) {}

Transition ConnectionStateMachine::connectRequested(TimePoint now) {
    (void)now;
    m_attempts = 0;
    m_lastReason = DiagnosisReason::None;
    m_dataDeadline.reset();
    m_lastPacketAt.reset();
    m_retryAt.reset();
    m_state = ConnState::Connecting;
    return {Action::OpenSource};
}

Transition ConnectionStateMachine::disconnectRequested(TimePoint now) {
    (void)now;
    m_attempts = 0;
    m_lastReason = DiagnosisReason::None;
    m_dataDeadline.reset();
    m_lastPacketAt.reset();
    m_retryAt.reset();
    m_state = ConnState::Idle;
    return {Action::CloseSource};
}

Transition ConnectionStateMachine::retryRequested(TimePoint now) {
    if (m_state != ConnState::Failed) return {};
    return connectRequested(now);
}

Transition ConnectionStateMachine::sessionOpened(TimePoint now) {
    if (m_state != ConnState::Connecting) return {};
    m_state = ConnState::SessionOpen;
    m_dataDeadline = now + m_stall.dataConfirmTimeout;
    return {};
}

Transition ConnectionStateMachine::packetReceived(TimePoint now) {
    if (m_state == ConnState::SessionOpen) {
        m_state = ConnState::Streaming;
        m_dataDeadline.reset();
    }
    if (m_state == ConnState::Streaming) {
        m_lastPacketAt = now;
    }
    return {};
}

Transition ConnectionStateMachine::framePresented(TimePoint now) {
    (void)now;
    if (m_state == ConnState::Streaming) {
        m_attempts = 0;                      // D2: 리셋의 유일한 자동 지점
        m_lastReason = DiagnosisReason::None;
    }
    return {};
}

// 실패 공통 처리: 한도 내면 retryState(Reconnecting 또는 Stalled)로, 초과면 Failed.
Transition ConnectionStateMachine::beginRetryOrFail(DiagnosisReason reason, ConnState retryState,
                                                    TimePoint now) {
    m_lastReason = reason;
    m_dataDeadline.reset();
    m_lastPacketAt.reset();
    ++m_attempts;
    if (m_attempts > m_reconnect.maxAttempts) {
        // D1(3차 리뷰): Failed = 고속 재시도 중지 + 저빈도 재시도 모드. 영구 정지가 아니다.
        m_state = ConnState::Failed;
        m_lastReason = DiagnosisReason::GaveUp;
        m_retryAt = now + m_reconnect.slowRetryDelay;
        return {Action::CloseSource};
    }
    m_state = retryState;
    m_retryAt = now + m_reconnect.retryDelay;
    return {Action::CloseSource};
}

Transition ConnectionStateMachine::errorOccurred(DiagnosisReason reason, TimePoint now) {
    switch (m_state) {
        case ConnState::Connecting:
        case ConnState::SessionOpen:
            return beginRetryOrFail(reason, ConnState::Reconnecting, now);
        case ConnState::Streaming:
            return beginRetryOrFail(reason, ConnState::Stalled, now);
        default:
            return {};  // Idle/Failed/재시도 대기 중 늦게 도착한 에러는 무시
    }
}

Transition ConnectionStateMachine::tick(TimePoint now) {
    switch (m_state) {
        case ConnState::SessionOpen:
            if (m_dataDeadline && now >= *m_dataDeadline) {
                // 가짜 연결: 세션은 열렸으나 패킷이 오지 않음. 카운터 유지 + 재시도.
                return beginRetryOrFail(DiagnosisReason::NoPackets, ConnState::Reconnecting, now);
            }
            return {};
        case ConnState::Streaming:
            if (m_lastPacketAt && now - *m_lastPacketAt >= m_stall.stallTimeout) {
                return beginRetryOrFail(DiagnosisReason::NoPackets, ConnState::Stalled, now);
            }
            return {};
        case ConnState::Reconnecting:
        case ConnState::Stalled:
        case ConnState::Failed:   // 저빈도 재시도 (retryAt이 slowRetryDelay로 잡혀 있음)
            if (m_retryAt && now >= *m_retryAt) {
                m_state = ConnState::Connecting;
                m_retryAt.reset();
                return {Action::OpenSource};
            }
            return {};
        default:
            return {};  // Idle, Connecting: tick으로 변하지 않음
    }
}

Transition ConnectionStateMachine::sourceAvailableHint(TimePoint now) {
    (void)now;
    if (m_state != ConnState::Failed) return {};  // 정상 재시도 사이클은 영향받지 않는다
    m_state = ConnState::Connecting;
    m_retryAt.reset();
    return {Action::OpenSource};
}

} // namespace nv::domain
```

- [ ] **Step 5: src/CMakeLists.txt를 STATIC 라이브러리로 전환**

```cmake
add_library(nv_domain STATIC
  domain/connection/ConnectionStateMachine.cpp
)
target_include_directories(nv_domain PUBLIC ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 6: 테스트 통과 확인**

Run: `cmake -B build -S . && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 전체 PASS

- [ ] **Step 7: Commit**

```bash
git add src/domain/connection src/CMakeLists.txt tests/
git commit -m "feat(domain): ConnectionStateMachine 정상 경로 — 세션열림≠연결확정, 표시확정시에만 카운터 리셋"
```

---

### Task 4: ConnectionStateMachine — 가짜 연결 회귀 (기존 버그의 박제)

**Files:**
- Test: `tests/unit/ConnectionStateMachineTest.cpp` (추가)

이 태스크는 구현 변경이 없어야 정상이다 — Task 3 구현이 이미 규칙을 담고 있는지 회귀 테스트로 검증한다. 실패하면 Task 3 구현의 버그이므로 수정한다.

- [ ] **Step 1: 가짜 연결 회귀 테스트 추가**

```cpp
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

TEST_CASE("회귀: 가짜 연결 30회 초과 시 Failed로 수렴한다 (give-up 보장)") {
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

// 가짜연결 사이클을 반복해 Failed까지 도달시키고 도달 시각을 반환하는 헬퍼.
// 익명 namespace의 makeSm()/t0 아래에 추가한다.
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
```

파일 상단 using에 `using namespace std::chrono_literals;`가 이미 있으므로 `1h` 사용 가능.

- [ ] **Step 2: 테스트 실행**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 전체 PASS. 실패하는 케이스가 있으면 ConnectionStateMachine.cpp의 해당 전이를 수정한다 (테스트가 명세다 — 테스트를 고치지 말 것).

- [ ] **Step 3: Commit**

```bash
git add tests/unit/ConnectionStateMachineTest.cpp
git commit -m "test(domain): 가짜연결 무한루프·give-up 미발동 버그를 회귀 테스트로 박제"
```

---

### Task 5: ConnectionStateMachine — stall 감지와 경계 케이스

**Files:**
- Test: `tests/unit/ConnectionStateMachineTest.cpp` (추가)

- [ ] **Step 1: stall·경계 테스트 추가**

```cpp
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
```

- [ ] **Step 2: 테스트 실행**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 전체 PASS (실패 시 구현 수정 — 테스트가 명세)

- [ ] **Step 3: Commit**

```bash
git add tests/unit/ConnectionStateMachineTest.cpp
git commit -m "test(domain): stall 감지·늦은 이벤트·Idle 경계 케이스 검증"
```

---

### Task 6: 앱 포트 정의 + 테스트 페이크

**Files:**
- Create: `src/app/ports/IClock.h`, `src/app/ports/ILogger.h`, `src/app/ports/IStreamSource.h`
- Create: `tests/helpers/FakeClock.h`, `tests/helpers/FakeStreamSource.h`, `tests/helpers/FakeLogger.h`

포트는 인터페이스뿐이라 테스트는 Task 7(ChannelController)에서 페이크와 함께 검증된다. 이 태스크는 컴파일 확인까지.

- [ ] **Step 1: src/app/ports/IClock.h**

```cpp
#pragma once
#include <chrono>

namespace nv::app {

class IClock {
public:
    virtual ~IClock() = default;
    virtual std::chrono::steady_clock::time_point now() const = 0;
};

} // namespace nv::app
```

- [ ] **Step 2: src/app/ports/ILogger.h**

```cpp
#pragma once
#include <string>
#include <string_view>
#include "src/domain/health/DiagnosisReason.h"

namespace nv::app {

enum class LogLevel { Debug, Info, Warn, Error };

// 설계 §6 관측성: 모든 로그는 채널ID·컴포넌트·원인코드를 구조화 필드로 갖는다.
class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void log(LogLevel level, std::string_view channelId, std::string_view component,
                     std::string_view message,
                     nv::domain::DiagnosisReason reason = nv::domain::DiagnosisReason::None) = 0;
};

} // namespace nv::app
```

- [ ] **Step 3: src/app/ports/IStreamSource.h**

```cpp
#pragma once
#include <string>
#include "src/domain/health/DiagnosisReason.h"

namespace nv::app {

// 어댑터(FFmpeg 등)가 도메인 이벤트로 번역해 호출하는 리스너 (설계 D4).
// 호출 스레드는 어댑터 내부 스레드일 수 있다 — ChannelController 쪽에서 직렬화 책임.
// (M1-코어에서는 동기 호출 가정. 스레드 마샬링은 M1-파이프라인에서 어댑터 경계에 추가.)
class StreamSourceListener {
public:
    virtual ~StreamSourceListener() = default;
    virtual void onSessionOpened() = 0;
    virtual void onPacketReceived() = 0;
    virtual void onFrameDecoded() = 0;
    virtual void onFramePresented() = 0;
    virtual void onSourceError(nv::domain::DiagnosisReason reason) = 0;
};

class IStreamSource {
public:
    virtual ~IStreamSource() = default;
    virtual void open(const std::string& url, StreamSourceListener& listener) = 0;
    virtual void close() = 0;
};

} // namespace nv::app
```

- [ ] **Step 4: tests/helpers/FakeClock.h**

```cpp
#pragma once
#include "src/app/ports/IClock.h"

namespace nv::test {

class FakeClock final : public nv::app::IClock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    std::chrono::steady_clock::time_point now() const override { return m_now; }
    void advance(std::chrono::milliseconds d) { m_now += d; }
    void setTo(TimePoint t) { m_now = t; }

private:
    TimePoint m_now{};  // 임의 epoch에서 시작
};

} // namespace nv::test
```

- [ ] **Step 5: tests/helpers/FakeStreamSource.h**

```cpp
#pragma once
#include <string>
#include <vector>
#include "src/app/ports/IStreamSource.h"

namespace nv::test {

// open/close 호출을 기록하고, 테스트가 리스너에 이벤트를 직접 주입하게 한다.
class FakeStreamSource final : public nv::app::IStreamSource {
public:
    void open(const std::string& url, nv::app::StreamSourceListener& listener) override {
        ++openCount;
        lastUrl = url;
        m_listener = &listener;
    }
    void close() override {
        ++closeCount;
        m_listener = nullptr;
    }

    // 테스트 헬퍼: 어댑터가 이벤트를 올리는 상황을 흉내낸다.
    nv::app::StreamSourceListener* listener() { return m_listener; }
    bool isOpen() const { return m_listener != nullptr; }

    int openCount = 0;
    int closeCount = 0;
    std::string lastUrl;

private:
    nv::app::StreamSourceListener* m_listener = nullptr;
};

} // namespace nv::test
```

- [ ] **Step 6: tests/helpers/FakeLogger.h**

```cpp
#pragma once
#include <string>
#include <vector>
#include "src/app/ports/ILogger.h"

namespace nv::test {

class FakeLogger final : public nv::app::ILogger {
public:
    struct Entry {
        nv::app::LogLevel level;
        std::string channelId;
        std::string component;
        std::string message;
        nv::domain::DiagnosisReason reason;
    };

    void log(nv::app::LogLevel level, std::string_view channelId, std::string_view component,
             std::string_view message, nv::domain::DiagnosisReason reason) override {
        entries.push_back({level, std::string(channelId), std::string(component),
                           std::string(message), reason});
    }

    std::vector<Entry> entries;
};

} // namespace nv::test
```

- [ ] **Step 7: 컴파일 확인 (헤더 인클루드 검증용으로 SmokeTest.cpp에 인클루드 추가)**

`tests/unit/SmokeTest.cpp`를 다음으로 교체:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "tests/helpers/FakeClock.h"
#include "tests/helpers/FakeStreamSource.h"
#include "tests/helpers/FakeLogger.h"

TEST_CASE("smoke: 포트와 페이크가 컴파일된다") {
    nv::test::FakeClock clock;
    clock.advance(std::chrono::milliseconds(100));
    nv::test::FakeStreamSource source;
    nv::test::FakeLogger logger;
    CHECK(source.openCount == 0);
    CHECK(logger.entries.empty());
    CHECK(clock.now().time_since_epoch().count() > 0);
}
```

tests/CMakeLists.txt의 `target_include_directories`에 `${CMAKE_SOURCE_DIR}`가 이미 있으므로 `tests/...` 인클루드가 동작한다.

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 전체 PASS

- [ ] **Step 8: Commit**

```bash
git add src/app/ports tests/helpers tests/unit/SmokeTest.cpp
git commit -m "feat(app): 포트 인터페이스(IClock/ILogger/IStreamSource) + 테스트 페이크"
```

---

### Task 7: ChannelController — 오케스트레이션과 StreamHealth 산출

**Files:**
- Create: `src/app/ChannelController.h`, `src/app/ChannelController.cpp`
- Modify: `src/CMakeLists.txt` (nv_app 라이브러리 추가)
- Test: `tests/unit/ChannelControllerTest.cpp`

- [ ] **Step 1: 실패하는 테스트 작성**

```cpp
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
```

tests/CMakeLists.txt의 `add_executable`에 `unit/ChannelControllerTest.cpp` 추가.

- [ ] **Step 2: 컴파일 실패 확인**

Run: `cmake --build build -j`
Expected: FAIL — `ChannelController.h: No such file`

- [ ] **Step 3: src/app/ChannelController.h 구현**

```cpp
#pragma once
#include <string>
#include "src/domain/connection/ConnectionStateMachine.h"
#include "src/domain/health/StreamHealth.h"
#include "ports/IClock.h"
#include "ports/ILogger.h"
#include "ports/IStreamSource.h"

namespace nv::app {

// 채널 하나의 오케스트레이터: 상태머신의 Action을 포트 호출로 실행하고
// 진단 6단계(StreamHealth)를 유지한다.
// 전용 control 스레드에서만 사용 (설계 D6, 3차 리뷰 — UI 스레드 아님. 단일 스레드 보장이 핵심).
class ChannelController final : public StreamSourceListener {
public:
    ChannelController(std::string channelId, std::string url,
                      IStreamSource& source, const IClock& clock, ILogger& logger,
                      nv::domain::ReconnectPolicy reconnect, nv::domain::StallPolicy stall);

    void connect();
    void disconnect();
    void retry();                  // Failed에서 수동 재시도 (카운터 리셋)
    void notifySourceAvailable();  // relay 헬스체크의 소스 복귀 신호 (M4에서 배선, D1)
    void tick();                   // 1초 주기로 호출

    nv::domain::ConnState state() const { return m_sm.state(); }
    const nv::domain::StreamHealth& health() const { return m_health; }
    int reconnectAttempts() const { return m_sm.reconnectAttempts(); }

    // StreamSourceListener (어댑터 → 도메인 이벤트)
    void onSessionOpened() override;
    void onPacketReceived() override;
    void onFrameDecoded() override;
    void onFramePresented() override;
    void onSourceError(nv::domain::DiagnosisReason reason) override;

private:
    void apply(const nv::domain::Transition& t);
    void logTransition(std::string_view trigger);
    bool sourceAlive() const { return m_sourceAlive; }

    std::string m_channelId;
    std::string m_url;
    IStreamSource& m_source;
    const IClock& m_clock;
    ILogger& m_logger;
    nv::domain::ConnectionStateMachine m_sm;
    nv::domain::StreamHealth m_health;
    bool m_sourceAlive = false;   // close 이후 늦은 이벤트 차단
};

} // namespace nv::app
```

- [ ] **Step 4: src/app/ChannelController.cpp 구현**

```cpp
#include "ChannelController.h"

namespace nv::app {

using nv::domain::Action;
using nv::domain::ConnState;
using nv::domain::DiagnosisReason;
using nv::domain::HealthStage;
using nv::domain::Transition;

ChannelController::ChannelController(std::string channelId, std::string url,
                                     IStreamSource& source, const IClock& clock, ILogger& logger,
                                     nv::domain::ReconnectPolicy reconnect,
                                     nv::domain::StallPolicy stall)
    : m_channelId(std::move(channelId)), m_url(std::move(url)),
      m_source(source), m_clock(clock), m_logger(logger), m_sm(reconnect, stall) {
    // M1은 직결 모드 — relay 단계는 측정 대상이 아님 (M4에서 모드별 설정으로 바뀐다)
    m_health.markNotApplicable(HealthStage::RelayIntake);
}

void ChannelController::apply(const Transition& t) {
    switch (t.action) {
        case Action::OpenSource:
            m_health.reset();
            m_sourceAlive = true;
            m_source.open(m_url, *this);
            break;
        case Action::CloseSource:
            m_sourceAlive = false;
            m_source.close();
            break;
        case Action::None:
            break;
    }
}

void ChannelController::logTransition(std::string_view trigger) {
    m_logger.log(LogLevel::Info, m_channelId, "ChannelController",
                 std::string(trigger) + " -> " + std::string(toString(m_sm.state())),
                 m_sm.lastReason());
}

void ChannelController::connect() {
    apply(m_sm.connectRequested(m_clock.now()));
    logTransition("connect");
}

void ChannelController::disconnect() {
    apply(m_sm.disconnectRequested(m_clock.now()));
    logTransition("disconnect");
}

void ChannelController::retry() {
    apply(m_sm.retryRequested(m_clock.now()));
    logTransition("retry");
}

void ChannelController::notifySourceAvailable() {
    const auto before = m_sm.state();
    apply(m_sm.sourceAvailableHint(m_clock.now()));
    if (m_sm.state() != before) logTransition("sourceAvailable");
}

void ChannelController::tick() {
    const auto before = m_sm.state();
    auto t = m_sm.tick(m_clock.now());
    if (m_sm.state() != before) {
        // 시간 기반 실패(가짜연결/stall)는 패킷 단계 실패로 기록
        if (m_sm.state() == ConnState::Reconnecting || m_sm.state() == ConnState::Stalled ||
            m_sm.state() == ConnState::Failed) {
            m_health.markFailed(HealthStage::PacketFlow, m_sm.lastReason());
        }
        logTransition("tick");
    }
    apply(t);
}

void ChannelController::onSessionOpened() {
    if (!m_sourceAlive) return;
    apply(m_sm.sessionOpened(m_clock.now()));
    if (m_sm.state() == ConnState::SessionOpen) {
        // 세션이 열렸다는 것은 장비(또는 relay)까지 도달했다는 뜻
        m_health.markReached(HealthStage::RtspSession);
        logTransition("sessionOpened");
    }
}

void ChannelController::onPacketReceived() {
    if (!m_sourceAlive) return;
    apply(m_sm.packetReceived(m_clock.now()));
    m_health.markReached(HealthStage::PacketFlow);
}

void ChannelController::onFrameDecoded() {
    if (!m_sourceAlive) return;
    m_health.markReached(HealthStage::Decoding);   // 상태머신 전이 없음 — 진단 전용
}

void ChannelController::onFramePresented() {
    if (!m_sourceAlive) return;
    apply(m_sm.framePresented(m_clock.now()));
    m_health.markReached(HealthStage::Presenting);
}

void ChannelController::onSourceError(DiagnosisReason reason) {
    if (!m_sourceAlive) return;
    const auto stage = (m_sm.state() == ConnState::Streaming) ? HealthStage::PacketFlow
                                                              : HealthStage::RtspSession;
    apply(m_sm.errorOccurred(reason, m_clock.now()));
    m_health.markFailed(stage, reason);
    logTransition("sourceError");
}

} // namespace nv::app
```

- [ ] **Step 5: src/CMakeLists.txt에 nv_app 추가**

```cmake
add_library(nv_domain STATIC
  domain/connection/ConnectionStateMachine.cpp
)
target_include_directories(nv_domain PUBLIC ${CMAKE_SOURCE_DIR})

add_library(nv_app STATIC
  app/ChannelController.cpp
)
target_include_directories(nv_app PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(nv_app PUBLIC nv_domain)
```

tests/CMakeLists.txt 링크에 `nv_app` 추가:

```cmake
target_link_libraries(nv_unit_tests PRIVATE nv_domain nv_app Catch2::Catch2WithMain)
```

- [ ] **Step 6: 테스트 통과 확인**

Run: `cmake -B build -S . && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 전체 PASS

주의 — "가짜 연결 시나리오" 테스트의 `closeCount == 1` 검증: `tick()`에서 상태가 바뀐 뒤 `apply(t)`가 호출되어 CloseSource가 실행된다. 또한 health 리셋 시점은 `Action::OpenSource` 처리 안(재접속 사이클 시작)이다 — 테스트의 "재오픈 후 RtspSession == Unknown"이 이를 검증한다.

- [ ] **Step 7: Commit**

```bash
git add src/app src/CMakeLists.txt tests/
git commit -m "feat(app): ChannelController — 상태머신 오케스트레이션 + 진단 6단계 산출"
```

---

### Task 8: 마무리 — README와 셀프 체크

**Files:**
- Create: `README.md`

- [ ] **Step 1: README.md 작성**

```markdown
# new_viewer

RTSP 기반 다채널 영상 관제 (재작성). 설계: `docs/2026-06-12-new-viewer-design.md`

## 빌드 & 테스트

​```bash
cmake -B build -S .
cmake --build build -j
ctest --test-dir build --output-on-failure
​```

요구사항: CMake ≥ 3.24, C++20 컴파일러. Catch2는 빌드 시 자동 다운로드(FetchContent).

## 현재 상태

- [x] M1-코어: 도메인 상태머신 + 진단 모델 + ChannelController (FFmpeg/Qt 의존 없음)
- [ ] M1-파이프라인: FFmpeg 어댑터 + 최소 UI + 통합테스트 + 실장비 소크
```

(코드펜스의 ​` 이스케이프는 실제 파일에서는 일반 ``` 로 작성한다.)

- [ ] **Step 2: 전체 테스트 최종 실행**

Run: `rm -rf build && cmake -B build -S . && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 클린 빌드에서 전체 PASS (케이스 약 25개)

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: README — 빌드 방법과 M1 진행 상태"
```

---

## 완료 기준 (이 플랜의 Definition of Done)

1. `ctest` 전체 통과 — 상태머신 전이(정상/가짜연결/stall/저빈도 재시도 전환/헬스 부활/수동복구/경계) 전수 검증
2. 기존 viewer의 2026-06-09 버그("가짜 연결 카운터 리셋 → give-up 무력화 → 무한루프")가 회귀 테스트로 박제되어 있음 (Task 4)
3. domain/app 코드에 Qt·FFmpeg 인클루드가 하나도 없음 (`grep -r "include <Q\|include.*libav" src/domain src/app` → 결과 없음)
4. 다음 플랜(M1-파이프라인)이 구현할 인터페이스(IStreamSource/StreamSourceListener)가 확정됨
