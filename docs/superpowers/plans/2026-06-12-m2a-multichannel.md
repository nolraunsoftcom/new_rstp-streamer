# M2a: 멀티채널 기능 코어 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 1채널 코어(M1) 위에 멀티채널 그리드를 올린다 — 채널 추가/삭제/수정/배치, 자동 저장·복원, 최대 20채널 동시 재생(SW 디코딩 유지). 완료 시 "기능은 다 되는 앱"이 나온다.

**Architecture:** 채널마다 독립 번들(소스+컨트롤러+프레임슬롯) — 한 채널의 추가/삭제/장애가 다른 채널에 영향을 주는 경로가 없어야 한다(설계 R2 채널 독립). control 스레드는 전체에 하나(ControlExecutor 공유), 도메인 그리드 규칙은 순수 함수. 영속화는 JSON 파일.

**Tech Stack:** M1과 동일 + Qt6 Core(JSON, infra/persist에 한정 — Widgets는 ui만). 성능 작업(HW 디코딩, GPU 렌더, Windows, 게이트 측정)은 **M2b 플랜** 범위.

**기준 부하 (사용자 확정):** H.264 640x480@30fps × 20채널, MediaMTX + ffmpeg publish 시뮬레이션.

**추가 요구 (2026-06-12 사용자):**
1. **UI 패리티** — 인터페이스는 기존 `../viewer/`와 동일해야 한다. 앱명 **"영상관리시스템"**, 아이콘(`logo.icns/ico/png`), 좌측 채널 패널 + 중앙 "전체" 탭 그리드(검정 배경) + 우측 고정폭 패널(설정/파일/로그 탭, #f5f5f5) + 패널 토글 버튼 + 상태바(채널 집계·CPU·메모리) 구조 그대로. 구현 시 `../viewer/src/MainWindow.cpp`와 `../viewer/src/Style.h`를 **시각적 레퍼런스로 직접 읽고** 이식하되, 비즈니스 로직은 절대 가져오지 않는다(룩앤필만 — 로직은 새 아키텍처).
2. **채널 수 정책** — 20은 하드 한도가 아니라 **성능 보장 기준선**이다. 소프트 한도 32(생성자 주입으로 조정 가능), 20채널은 성능 게이트로 보장, 21~32는 동작 보장+성능 best-effort(M2b HW 디코딩 후 재평가).

**브랜치:** `m2a-multichannel` (main에서 분기)

---

## 파일 구조

```
src/
  domain/
    layout/GridRules.h               Auto 컬럼 규칙 (순수 함수)
    channel/ChannelConfig.h          채널 설정 값 객체
  app/
    ports/IChannelRepository.h       채널 목록 영속화 포트
    ports/IChannelRuntimeFactory.h   채널별 스트림 소스 생성 포트
    ChannelManager.h/.cpp            채널 N개 수명·배치·영속 오케스트레이션
  infra/
    persist/JsonChannelRepository.h/.cpp   QJson 기반 저장/복원
    ffmpeg/ChannelSourceFactory.h/.cpp     FFmpeg 소스+슬롯+마샬링 번들 생성, 슬롯 레지스트리
  ui/
    grid/GridView.h/.cpp             타일 그리드 (컬럼 규칙 적용, 재구성)
    channels/ChannelDialog.h/.cpp    채널 추가/수정 다이얼로그
    shell/MainWindow.h/.cpp          (개편) 툴바+그리드, 다채널 스냅샷 라우팅
    shell/ControlBridge.h            (수정) channelId 포함 시그널
tests/
  unit/GridRulesTest.cpp  unit/ChannelManagerTest.cpp  unit/JsonChannelRepositoryTest.cpp
  helpers/FakeChannelRepository.h  helpers/FakeRuntimeFactory.h
ops/
  sim-20ch.sh                        MediaMTX + 퍼블리셔 20개 시뮬 기동
  measure-baseline.sh                20ch SW 베이스라인 측정 (CPU/RSS/드롭)
```

---

### Task 1: GridRules (도메인 순수 함수)

**Files:** Create `src/domain/layout/GridRules.h`; Test `tests/unit/GridRulesTest.cpp`

- [ ] **Step 1: 테스트 먼저**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "src/domain/layout/GridRules.h"

using nv::domain::grid::autoColumns;
using nv::domain::grid::rowsFor;

TEST_CASE("Auto 컬럼 규칙 (설계 §2)") {
    CHECK(autoColumns(0) == 3);
    CHECK(autoColumns(1) == 1);
    CHECK(autoColumns(2) == 2);
    CHECK(autoColumns(4) == 2);
    CHECK(autoColumns(5) == 3);
    CHECK(autoColumns(9) == 3);
    CHECK(autoColumns(10) == 4);
    CHECK(autoColumns(16) == 4);
    CHECK(autoColumns(17) == 5);
    CHECK(autoColumns(20) == 5);
}

TEST_CASE("행 수 계산") {
    CHECK(rowsFor(0, 3) == 1);    // 빈 그리드도 1행 (No Stream 표시 공간)
    CHECK(rowsFor(1, 1) == 1);
    CHECK(rowsFor(4, 2) == 2);
    CHECK(rowsFor(5, 3) == 2);
    CHECK(rowsFor(20, 5) == 4);
}
```

- [ ] **Step 2: 구현 — `src/domain/layout/GridRules.h`**

```cpp
#pragma once

namespace nv::domain::grid {

// 설계 §2 Auto 컬럼 규칙. 순수 함수 — UI는 이 값을 그대로 사용한다.
constexpr int autoColumns(int channelCount) {
    if (channelCount <= 0) return 3;
    if (channelCount == 1) return 1;
    if (channelCount <= 4) return 2;
    if (channelCount <= 9) return 3;
    if (channelCount <= 16) return 4;
    return 5;
}

constexpr int rowsFor(int channelCount, int columns) {
    if (channelCount <= 0 || columns <= 0) return 1;
    return (channelCount + columns - 1) / columns;
}

} // namespace nv::domain::grid
```

- [ ] **Step 3:** 테스트 등록(tests/CMakeLists.txt) → 전체 PASS → Commit: `feat(domain): GridRules — Auto 컬럼/행 규칙`

---

### Task 2: ChannelConfig + 포트 + 페이크

**Files:** Create `src/domain/channel/ChannelConfig.h`, `src/app/ports/IChannelRepository.h`, `src/app/ports/IChannelRuntimeFactory.h`, `tests/helpers/FakeChannelRepository.h`, `tests/helpers/FakeRuntimeFactory.h`

- [ ] **Step 1: `src/domain/channel/ChannelConfig.h`**

```cpp
#pragma once
#include <string>

namespace nv::domain {

struct ChannelConfig {
    std::string id;        // 불변 고유 식별자 ("ch<n>") — 생성 시 부여
    std::string name;
    std::string url;
    int gridIndex = -1;    // 그리드 셀 위치. -1 = 미배치(manager가 부여)

    bool operator==(const ChannelConfig&) const = default;
};

} // namespace nv::domain
```

- [ ] **Step 2: `src/app/ports/IChannelRepository.h`**

```cpp
#pragma once
#include <vector>
#include "src/domain/channel/ChannelConfig.h"

namespace nv::app {

class IChannelRepository {
public:
    virtual ~IChannelRepository() = default;
    virtual std::vector<nv::domain::ChannelConfig> load() = 0;
    virtual void save(const std::vector<nv::domain::ChannelConfig>& channels) = 0;
};

} // namespace nv::app
```

- [ ] **Step 3: `src/app/ports/IChannelRuntimeFactory.h`**

```cpp
#pragma once
#include <memory>
#include <string>
#include "IStreamSource.h"

namespace nv::app {

// 채널별 스트림 소스 생성. 구현(infra)은 프레임 슬롯 등록·마샬링 래핑까지 책임진다.
// 반환된 소스는 호출자(ChannelManager)가 수명을 소유한다.
class IChannelRuntimeFactory {
public:
    virtual ~IChannelRuntimeFactory() = default;
    virtual std::unique_ptr<IStreamSource> createSource(const std::string& channelId) = 0;
    virtual void destroySource(const std::string& channelId) = 0;  // 슬롯 등 부속 정리
};

} // namespace nv::app
```

- [ ] **Step 4: `tests/helpers/FakeChannelRepository.h`**

```cpp
#pragma once
#include "src/app/ports/IChannelRepository.h"

namespace nv::test {

class FakeChannelRepository final : public nv::app::IChannelRepository {
public:
    std::vector<nv::domain::ChannelConfig> load() override { ++loadCount; return stored; }
    void save(const std::vector<nv::domain::ChannelConfig>& channels) override {
        ++saveCount;
        stored = channels;
    }
    std::vector<nv::domain::ChannelConfig> stored;
    int loadCount = 0;
    int saveCount = 0;
};

} // namespace nv::test
```

- [ ] **Step 5: `tests/helpers/FakeRuntimeFactory.h`**

```cpp
#pragma once
#include <map>
#include "src/app/ports/IChannelRuntimeFactory.h"
#include "FakeStreamSource.h"

namespace nv::test {

// 채널별 FakeStreamSource를 만들어주고, 테스트가 채널ID로 접근할 수 있게 한다.
class FakeRuntimeFactory final : public nv::app::IChannelRuntimeFactory {
public:
    // 소유권은 ChannelManager로 가지만, 테스트 관찰용 포인터를 registry에 남긴다.
    std::unique_ptr<nv::app::IStreamSource> createSource(const std::string& channelId) override {
        auto src = std::make_unique<FakeStreamSource>();
        registry[channelId] = src.get();
        ++createCount;
        return src;
    }
    void destroySource(const std::string& channelId) override {
        registry.erase(channelId);
        ++destroyCount;
    }

    std::map<std::string, FakeStreamSource*> registry;
    int createCount = 0;
    int destroyCount = 0;
};

} // namespace nv::test
```

- [ ] **Step 6:** SmokeTest에 페이크 2종 인스턴스화 한 줄씩 추가해 컴파일 검증 → 전체 PASS → Commit: `feat(app): 채널 영속화·런타임 팩토리 포트 + 페이크`

---

### Task 3: ChannelManager (핵심 — TDD)

**Files:** Create `src/app/ChannelManager.h/.cpp`; Test `tests/unit/ChannelManagerTest.cpp`; Modify `src/CMakeLists.txt`(nv_app 소스 추가), `tests/CMakeLists.txt`

- [ ] **Step 1: 테스트 먼저 — `tests/unit/ChannelManagerTest.cpp`**

```cpp
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
```

- [ ] **Step 2: 구현 — `src/app/ChannelManager.h`**

```cpp
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "ChannelController.h"
#include "ports/IChannelRepository.h"
#include "ports/IChannelRuntimeFactory.h"

namespace nv::app {

// 채널 N개의 수명·배치·영속을 관리한다. 모든 메서드는 control 스레드에서만 호출 (설계 D6).
// 채널 독립(설계 R2): 한 채널의 추가/삭제/장애가 다른 채널의 소스·컨트롤러를 건드리지 않는다.
class ChannelManager {
public:
    static constexpr int kPerformanceTargetChannels = 20;  // 성능 게이트 기준선 (M2b)
    static constexpr int kDefaultMaxChannels = 32;         // 소프트 한도 (주입으로 조정 가능)

    ChannelManager(IChannelRepository& repo, IChannelRuntimeFactory& factory,
                   const IClock& clock, ILogger& logger,
                   nv::domain::ReconnectPolicy reconnect, nv::domain::StallPolicy stall,
                   int maxChannels = kDefaultMaxChannels);

    void restore(bool autoConnect);                        // 저장본 로드 (시작 시 1회)
    std::string addChannel(std::string name, std::string url);   // 실패(한도) 시 "" 반환
    void removeChannel(const std::string& id);
    void updateChannel(const std::string& id, std::string name, std::string url);
    void swapGrid(const std::string& idA, const std::string& idB);

    void connectAll();
    void disconnectAll();
    void tickAll();

    int channelCount() const { return static_cast<int>(m_entries.size()); }
    std::vector<nv::domain::ChannelConfig> configs() const;       // gridIndex 순 정렬
    const nv::domain::ChannelConfig& config(const std::string& id) const;
    ChannelController* controller(const std::string& id);

    void setListChangedObserver(std::function<void()> obs) { m_listChanged = std::move(obs); }

private:
    struct Entry {
        nv::domain::ChannelConfig cfg;
        std::unique_ptr<IStreamSource> source;     // factory 산출물 (먼저 선언 — ctrl보다 오래 산다)
        std::unique_ptr<ChannelController> ctrl;
    };

    Entry& makeEntry(nv::domain::ChannelConfig cfg);
    void persist();
    void notifyList();
    int nextGridIndex() const;
    int nextIdNumber() const;

    IChannelRepository& m_repo;
    IChannelRuntimeFactory& m_factory;
    const IClock& m_clock;
    ILogger& m_logger;
    nv::domain::ReconnectPolicy m_reconnect;
    nv::domain::StallPolicy m_stall;
    std::map<std::string, Entry> m_entries;        // id → entry
    std::function<void()> m_listChanged;
    int m_maxChannels;
};

} // namespace nv::app
```

- [ ] **Step 3: `src/app/ChannelManager.cpp`**

```cpp
#include "ChannelManager.h"
#include <algorithm>

namespace nv::app {

using nv::domain::ChannelConfig;

ChannelManager::ChannelManager(IChannelRepository& repo, IChannelRuntimeFactory& factory,
                               const IClock& clock, ILogger& logger,
                               nv::domain::ReconnectPolicy reconnect,
                               nv::domain::StallPolicy stall, int maxChannels)
    : m_repo(repo), m_factory(factory), m_clock(clock), m_logger(logger),
      m_reconnect(reconnect), m_stall(stall), m_maxChannels(maxChannels) {}

ChannelManager::Entry& ChannelManager::makeEntry(ChannelConfig cfg) {
    auto source = m_factory.createSource(cfg.id);
    auto ctrl = std::make_unique<ChannelController>(cfg.id, cfg.url, *source, m_clock, m_logger,
                                                    m_reconnect, m_stall);
    auto [it, ok] = m_entries.emplace(cfg.id,
                                      Entry{std::move(cfg), std::move(source), std::move(ctrl)});
    return it->second;
}

void ChannelManager::restore(bool autoConnect) {
    for (auto& cfg : m_repo.load()) {
        if (channelCount() >= m_maxChannels) break;
        makeEntry(cfg);
    }
    if (autoConnect) connectAll();
    notifyList();
}

int ChannelManager::nextGridIndex() const {
    int idx = 0;
    for (bool taken = true; taken; ++idx) {
        taken = false;
        for (auto& [_, e] : m_entries)
            if (e.cfg.gridIndex == idx) { taken = true; break; }
        if (!taken) return idx;
    }
    return idx;
}

int ChannelManager::nextIdNumber() const {
    int maxN = 0;
    for (auto& [id, _] : m_entries) {
        if (id.rfind("ch", 0) == 0) maxN = std::max(maxN, std::atoi(id.c_str() + 2));
    }
    return maxN + 1;
}

std::string ChannelManager::addChannel(std::string name, std::string url) {
    if (channelCount() >= m_maxChannels) return {};
    ChannelConfig cfg;
    cfg.id = "ch" + std::to_string(nextIdNumber());
    cfg.name = std::move(name);
    cfg.url = std::move(url);
    cfg.gridIndex = nextGridIndex();
    makeEntry(std::move(cfg));
    persist();
    notifyList();
    return std::prev(m_entries.end())->first;   // 주의: map 순서 ≠ 삽입 순서. 아래 참고.
}
```

주의: `std::map`은 키 정렬이라 `std::prev(end())`가 방금 넣은 항목이 아닐 수 있다. `addChannel`은 `cfg.id`를 지역 변수로 보관했다가 그것을 반환하도록 구현하라 (위 코드의 마지막 줄을 `return cfg.id;`가 되도록 — `makeEntry(std::move(cfg))` 전에 `const std::string id = cfg.id;` 보관). 테스트가 이를 검증한다.

```cpp
void ChannelManager::removeChannel(const std::string& id) {
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return;
    it->second.ctrl->disconnect();          // 소스 정리 (TEARDOWN 경유)
    m_entries.erase(it);                    // ctrl → source 순 파괴
    m_factory.destroySource(id);
    persist();
    notifyList();
}

void ChannelManager::updateChannel(const std::string& id, std::string name, std::string url) {
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return;
    auto& e = it->second;
    e.cfg.name = std::move(name);
    const bool urlChanged = (e.cfg.url != url);
    e.cfg.url = url;
    if (urlChanged) {
        e.ctrl->disconnect();               // Failed 함정 방지 — main.cpp 연결 버튼과 동일 시퀀스
        e.ctrl->setUrl(std::move(url));
        e.ctrl->connect();
    }
    persist();
    notifyList();
}

void ChannelManager::swapGrid(const std::string& idA, const std::string& idB) {
    auto a = m_entries.find(idA);
    auto b = m_entries.find(idB);
    if (a == m_entries.end() || b == m_entries.end()) return;
    std::swap(a->second.cfg.gridIndex, b->second.cfg.gridIndex);
    persist();
    notifyList();
}

void ChannelManager::connectAll() {
    for (auto& [_, e] : m_entries) e.ctrl->connect();
}
void ChannelManager::disconnectAll() {
    for (auto& [_, e] : m_entries) e.ctrl->disconnect();
}
void ChannelManager::tickAll() {
    for (auto& [_, e] : m_entries) e.ctrl->tick();
}

std::vector<ChannelConfig> ChannelManager::configs() const {
    std::vector<ChannelConfig> out;
    out.reserve(m_entries.size());
    for (auto& [_, e] : m_entries) out.push_back(e.cfg);
    std::sort(out.begin(), out.end(),
              [](auto& x, auto& y) { return x.gridIndex < y.gridIndex; });
    return out;
}

const ChannelConfig& ChannelManager::config(const std::string& id) const {
    return m_entries.at(id).cfg;
}

ChannelController* ChannelManager::controller(const std::string& id) {
    auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : it->second.ctrl.get();
}

void ChannelManager::persist() {
    m_repo.save(configs());
}
void ChannelManager::notifyList() {
    if (m_listChanged) m_listChanged();
}

} // namespace nv::app
```

- [ ] **Step 4:** `src/CMakeLists.txt`의 nv_app 소스에 `app/ChannelManager.cpp` 추가, 테스트 등록 → 전체 PASS → Commit: `feat(app): ChannelManager — 멀티채널 수명·배치·영속 오케스트레이션`

---

### Task 4: JsonChannelRepository (Qt Core)

**Files:** Create `src/infra/persist/JsonChannelRepository.h/.cpp`; Test `tests/unit/JsonChannelRepositoryTest.cpp`; Modify `src/CMakeLists.txt`(nv_infra에 소스+Qt6::Core 링크), `tests/CMakeLists.txt`

- [ ] **Step 1: 테스트 먼저**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <QTemporaryDir>
#include "src/infra/persist/JsonChannelRepository.h"

using nv::domain::ChannelConfig;

TEST_CASE("저장→로드 라운드트립") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const std::string path = (dir.path() + "/channels.json").toStdString();

    nv::infra::JsonChannelRepository repo(path);
    std::vector<ChannelConfig> in = {
        {"ch1", "카메라1", "rtsp://169.254.4.1:8900/live", 0},
        {"ch2", "무전기", "rtsp://127.0.0.1:8554/radio", 3},
    };
    repo.save(in);

    nv::infra::JsonChannelRepository repo2(path);   // 새 인스턴스로 로드
    auto out = repo2.load();
    REQUIRE(out.size() == 2);
    CHECK(out[0] == in[0]);
    CHECK(out[1] == in[1]);
}

TEST_CASE("파일 없음 → 빈 목록 (첫 실행)") {
    QTemporaryDir dir;
    nv::infra::JsonChannelRepository repo((dir.path() + "/none.json").toStdString());
    CHECK(repo.load().empty());
}

TEST_CASE("손상된 JSON → 빈 목록 + 크래시 없음") {
    QTemporaryDir dir;
    const QString p = dir.path() + "/bad.json";
    { QFile f(p); f.open(QIODevice::WriteOnly); f.write("{not json!!"); }
    nv::infra::JsonChannelRepository repo(p.toStdString());
    CHECK(repo.load().empty());
}
```

- [ ] **Step 2: 구현 — `src/infra/persist/JsonChannelRepository.h`**

```cpp
#pragma once
#include <string>
#include "src/app/ports/IChannelRepository.h"

namespace nv::infra {

// 채널 목록 JSON 영속화. 쓰기는 임시파일+rename으로 원자적 (저장 중 크래시에도 파일 보존).
class JsonChannelRepository final : public nv::app::IChannelRepository {
public:
    explicit JsonChannelRepository(std::string filePath);
    std::vector<nv::domain::ChannelConfig> load() override;
    void save(const std::vector<nv::domain::ChannelConfig>& channels) override;

private:
    std::string m_path;
};

} // namespace nv::infra
```

- [ ] **Step 3: `src/infra/persist/JsonChannelRepository.cpp`**

```cpp
#include "JsonChannelRepository.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace nv::infra {

using nv::domain::ChannelConfig;

JsonChannelRepository::JsonChannelRepository(std::string filePath)
    : m_path(std::move(filePath)) {}

std::vector<ChannelConfig> JsonChannelRepository::load() {
    QFile f(QString::fromStdString(m_path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return {};

    std::vector<ChannelConfig> out;
    for (const auto& v : doc.array()) {
        const auto o = v.toObject();
        ChannelConfig c;
        c.id = o.value("id").toString().toStdString();
        c.name = o.value("name").toString().toStdString();
        c.url = o.value("url").toString().toStdString();
        c.gridIndex = o.value("gridIndex").toInt(-1);
        if (!c.id.empty()) out.push_back(std::move(c));
    }
    return out;
}

void JsonChannelRepository::save(const std::vector<ChannelConfig>& channels) {
    QJsonArray arr;
    for (const auto& c : channels) {
        QJsonObject o;
        o["id"] = QString::fromStdString(c.id);
        o["name"] = QString::fromStdString(c.name);
        o["url"] = QString::fromStdString(c.url);
        o["gridIndex"] = c.gridIndex;
        arr.append(o);
    }
    QSaveFile f(QString::fromStdString(m_path));   // 원자적 쓰기
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    f.commit();
}

} // namespace nv::infra
```

- [ ] **Step 4:** CMake — `src/CMakeLists.txt`의 nv_infra에 소스 추가 + `find_package(Qt6 ... COMPONENTS Widgets Core)` 확인 후:

```cmake
target_link_libraries(nv_infra PUBLIC nv_app PkgConfig::FFMPEG Qt6::Core)
```

tests에서 QTemporaryDir 사용을 위해 `target_link_libraries(nv_unit_tests PRIVATE ... Qt6::Core)` 추가. 전체 PASS → Commit: `feat(infra): JsonChannelRepository — 원자적 JSON 영속화`

(참고: infra의 Qt 사용은 **Core 한정** — Widgets는 ui 레이어만. 설계 의존 방향 유지.)

---

### Task 5: ChannelSourceFactory + 슬롯 레지스트리

**Files:** Create `src/infra/ffmpeg/ChannelSourceFactory.h/.cpp`; Modify `src/CMakeLists.txt`

- [ ] **Step 1: `src/infra/ffmpeg/ChannelSourceFactory.h`**

```cpp
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include "src/app/ports/IChannelRuntimeFactory.h"
#include "src/app/ports/IExecutor.h"
#include "src/app/MarshallingStreamSource.h"
#include "FfmpegStreamSource.h"
#include "src/infra/video/LatestFrameSlot.h"

namespace nv::infra {

// 채널별 (FfmpegStreamSource + LatestFrameSlot + Marshalling) 번들 생성.
// 슬롯은 레지스트리로 보관 — UI(타일)가 channelId로 조회한다.
// 슬롯 수명: destroySource 후에도 즉시 파괴하지 않고 보관(타일 폴링 경합 방지),
//            동일 id 재생성 시 재사용. (M2a 단순화 — 채널 최대 20개라 누수 아님)
class ChannelSourceFactory final : public nv::app::IChannelRuntimeFactory {
public:
    explicit ChannelSourceFactory(nv::app::IExecutor& executor) : m_executor(executor) {}

    std::unique_ptr<nv::app::IStreamSource> createSource(const std::string& channelId) override;
    void destroySource(const std::string& channelId) override;

    LatestFrameSlot* slot(const std::string& channelId);   // UI 조회용 (없으면 nullptr)

private:
    // 소유권을 한 덩어리로: Marshalling(외피) ← Ffmpeg(내부) ← slot(레지스트리 소유)
    class Bundle final : public nv::app::IStreamSource {
    public:
        Bundle(LatestFrameSlot& slot, nv::app::IExecutor& ex)
            : m_ffmpeg(slot), m_marshalled(m_ffmpeg, ex) {}
        void open(const std::string& url, nv::app::StreamSourceListener& l) override {
            m_marshalled.open(url, l);
        }
        void close() override { m_marshalled.close(); }

    private:
        FfmpegStreamSource m_ffmpeg;
        nv::app::MarshallingStreamSource m_marshalled;
    };

    nv::app::IExecutor& m_executor;
    std::mutex m_mu;                 // slot()은 UI 스레드, create/destroy는 control 스레드
    std::map<std::string, std::unique_ptr<LatestFrameSlot>> m_slots;
};

} // namespace nv::infra
```

- [ ] **Step 2: `src/infra/ffmpeg/ChannelSourceFactory.cpp`**

```cpp
#include "ChannelSourceFactory.h"

namespace nv::infra {

std::unique_ptr<nv::app::IStreamSource> ChannelSourceFactory::createSource(
    const std::string& channelId) {
    LatestFrameSlot* slot = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto& s = m_slots[channelId];
        if (!s) s = std::make_unique<LatestFrameSlot>();
        slot = s.get();
    }
    return std::make_unique<Bundle>(*slot, m_executor);
}

void ChannelSourceFactory::destroySource(const std::string& channelId) {
    (void)channelId;   // 슬롯은 의도적으로 보관 (헤더 주석 참조)
}

LatestFrameSlot* ChannelSourceFactory::slot(const std::string& channelId) {
    std::lock_guard lk(m_mu);
    auto it = m_slots.find(channelId);
    return it == m_slots.end() ? nullptr : it->second.get();
}

} // namespace nv::infra
```

- [ ] **Step 3:** CMake에 소스 추가, 빌드+기존 테스트 무파손 확인 → Commit: `feat(infra): ChannelSourceFactory — 채널별 소스 번들 + 슬롯 레지스트리`

---

### Task 6: ControlBridge 다채널화 + GridView + ChannelDialog + **레거시 UI 패리티 셸**

**Files:** Modify `src/ui/shell/ControlBridge.h`, `src/ui/shell/MainWindow.h/.cpp`, `src/ui/main.cpp`, 루트/`src/CMakeLists.txt`; Create `src/ui/grid/GridView.h/.cpp`, `src/ui/channels/ChannelDialog.h/.cpp`, `src/ui/channels/ChannelListPanel.h/.cpp`, `src/ui/shell/LogPanel.h/.cpp`, `src/infra/system/ResourceMonitor.h/.cpp`(레거시 이식), `resources/`(레거시 복사)

- [ ] **Step 0 — 레거시 패리티 필수 작업 (아래 스니펫들의 외피. `../viewer/src/MainWindow.cpp`·`Style.h`를 직접 읽고 룩앤필을 이식하되 로직은 가져오지 말 것):**

1. **리소스/아이덴티티**: `../viewer/resources/`(logo.icns/ico/png, resources.qrc, app.rc.in)를 `resources/`로 복사. CMake: qrc 추가(AUTORCC ON), macOS `MACOSX_BUNDLE` + icns(`MACOSX_PACKAGE_LOCATION Resources` + `MACOSX_BUNDLE_ICON_FILE`). main.cpp: `app.setApplicationName(QStringLiteral("영상관리시스템")); app.setWindowIcon(QIcon(":/logo.png"));` 윈도우 타이틀도 "영상관리시스템".
2. **3패널 레이아웃** (레거시 159~210행 구조): 좌측 채널 패널(`ChannelListPanel`) + 토글버튼 + 중앙 `QTabWidget`("전체" 탭에 GridView, pane 검정 배경 스타일시트 — 레거시 617~653행) + 토글버튼 + 우측 고정폭(`RIGHT_PANEL_WIDTH=320`) 패널(#f5f5f5).
3. **ChannelListPanel** (레거시 좌측 사이드바 대응): `QListWidget` — 항목에 채널명+상태 요약 표시, 선택/우클릭 메뉴(채널 수정/삭제/재시도 — GridView Callbacks 재사용), 하단 [추가]/[삭제] 버튼. 스냅샷 수신 시 해당 항목 상태 텍스트 갱신.
4. **우측 패널 탭 3개**: ① 설정 탭 — 그리드 컬럼 콤보(Auto/1~5) 이동 배치, ② 파일 탭 — 자리표시(빈 목록 + "스냅샷/녹화는 M3에서") , ③ 로그 탭 — `LogPanel`(QPlainTextEdit, 최대 2000줄 유지).
5. **로그 탭 배선**: `src/infra/system/CompositeLogger.h` 추가 — ILogger 구현, StderrLogger로 위임 + `std::function<void(QString)>` 콜백 호출(있으면). main.cpp에서 콜백→`QMetaObject::invokeMethod`(queued)→LogPanel append. (control/어댑터 스레드에서 호출되므로 반드시 queued)
6. **상태바** (레거시 하단 대응): 채널 집계("연결 n / 전체 m") + 시스템 CPU/메모리 — `../viewer/src/ResourceMonitor.cpp`(233줄)를 `src/infra/system/ResourceMonitor.h/.cpp`로 이식(네임스페이스 nv::infra로, Qt Core 타이머 기반 그대로). 1초 주기 갱신.
7. 패널 토글 동작: 좌/우 각각 독립 숨김/표시, 토글 후 그리드 재계산 (레거시 685~700행 동작 동일).

이하 Step 1~7의 스니펫은 이 셸 안에 들어가는 부품이다 — MainWindow 스니펫의 단순 툴바 구성 대신 위 3패널 구조를 적용하고, 컬럼 콤보는 우측 설정 탭으로 이동하라.

- [ ] **Step 1: `src/ui/shell/ControlBridge.h`** — 시그널에 channelId 추가:

```cpp
#pragma once
#include <QObject>
#include "src/app/ChannelSnapshot.h"

namespace nv::ui {

class ControlBridge : public QObject {
    Q_OBJECT
public:
    // control 스레드에서 호출
    void publish(const QString& channelId, const nv::app::ChannelSnapshot& s) {
        QList<int> stages;
        for (auto st : nv::domain::kAllHealthStages)
            stages.push_back(static_cast<int>(s.health.stageState(st)));
        emit snapshotChanged(channelId,
                             QString::fromUtf8(toString(s.state).data(),
                                               static_cast<int>(toString(s.state).size())),
                             s.attempts, stages, s.packetsPerSec,
                             static_cast<qlonglong>(s.msSinceLastPacket));
    }

signals:
    void snapshotChanged(QString channelId, QString state, int attempts, QList<int> stages,
                         double pps, qlonglong msSinceLastPacket);
};

} // namespace nv::ui
```

(reason 문자열은 M2a 타일 헤더에서 미표시 — 진단 패널 상세는 M3/M4에서 복귀. 단순화로 인한 의도적 생략.)

- [ ] **Step 2: `src/ui/grid/GridView.h`**

```cpp
#pragma once
#include <QWidget>
#include <functional>
#include <map>
#include "src/domain/channel/ChannelConfig.h"

class QGridLayout;

namespace nv::infra { class ChannelSourceFactory; }

namespace nv::ui {
class VideoTileWidget;

// 채널 타일 그리드. rebuild()로 전체 재구성 (M2a 단순화 — 드래그 스왑은 우클릭 메뉴로 대체,
// 드래그앤드롭 UX는 M3). 컬럼 0 = Auto.
class GridView : public QWidget {
    Q_OBJECT
public:
    struct Callbacks {
        std::function<void(std::string id)> editRequested;
        std::function<void(std::string id)> removeRequested;
        std::function<void(std::string id)> retryRequested;
        std::function<void(std::string id)> framePainted;
        std::function<void(std::string idA, std::string idB)> swapRequested;
    };

    GridView(nv::infra::ChannelSourceFactory& slots, Callbacks cb, QWidget* parent = nullptr);

    void rebuild(const std::vector<nv::domain::ChannelConfig>& configs, int manualColumns);
    void updateTileStatus(const QString& channelId, const QString& state, int attempts,
                          const QList<int>& stages, double pps, qlonglong msSince);

private:
    nv::infra::ChannelSourceFactory& m_slots;
    Callbacks m_cb;
    QGridLayout* m_grid = nullptr;
    struct Tile;                                  // VideoTileWidget + 헤더/상태 라벨 묶음
    std::map<QString, Tile*> m_tiles;             // channelId → tile
    std::vector<nv::domain::ChannelConfig> m_lastConfigs;
};

} // namespace nv::ui
```

- [ ] **Step 3: `src/ui/grid/GridView.cpp`**

```cpp
#include "GridView.h"
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>
#include "src/domain/layout/GridRules.h"
#include "src/infra/ffmpeg/ChannelSourceFactory.h"
#include "VideoTileWidget.h"

namespace nv::ui {

// 타일 = 헤더(이름·상태) + 영상 + 푸터(패킷 흐름)
struct GridView::Tile : QWidget {
    QLabel* header = nullptr;
    QLabel* flow = nullptr;
    VideoTileWidget* video = nullptr;
    std::string channelId;
    QString name;

    Tile(nv::infra::LatestFrameSlot& slot, std::string id, QString nm, QWidget* parent)
        : QWidget(parent), channelId(std::move(id)), name(std::move(nm)) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        lay->setSpacing(2);
        header = new QLabel(name, this);
        header->setStyleSheet(QStringLiteral("font-weight:bold"));
        video = new VideoTileWidget(slot, this);
        video->setMinimumSize(160, 120);
        flow = new QLabel(QStringLiteral("패킷 —"), this);
        flow->setStyleSheet(QStringLiteral("color:gray; font-size:11px"));
        lay->addWidget(header);
        lay->addWidget(video, 1);
        lay->addWidget(flow);
        setContextMenuPolicy(Qt::CustomContextMenu);
    }
};

GridView::GridView(nv::infra::ChannelSourceFactory& slots, Callbacks cb, QWidget* parent)
    : QWidget(parent), m_slots(slots), m_cb(std::move(cb)) {
    m_grid = new QGridLayout(this);
    m_grid->setSpacing(4);
}

void GridView::rebuild(const std::vector<nv::domain::ChannelConfig>& configs,
                       int manualColumns) {
    m_lastConfigs = configs;
    // 전체 철거 (타일 위젯 삭제 — 슬롯은 factory가 보관하므로 안전)
    while (auto* item = m_grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_tiles.clear();

    const int n = static_cast<int>(configs.size());
    const int cols = manualColumns > 0 ? manualColumns : nv::domain::grid::autoColumns(n);

    int cell = 0;
    for (const auto& cfg : configs) {
        auto* slot = m_slots.slot(cfg.id);
        if (slot == nullptr) continue;
        auto* tile = new Tile(*slot, cfg.id, QString::fromStdString(cfg.name), this);
        m_grid->addWidget(tile, cell / cols, cell % cols);
        m_tiles[QString::fromStdString(cfg.id)] = tile;
        ++cell;

        connect(tile->video, &VideoTileWidget::framePainted, this,
                [this, id = cfg.id] { m_cb.framePainted(id); });
        connect(tile, &QWidget::customContextMenuRequested, this, [this, tile](const QPoint& p) {
            QMenu menu;
            auto* actEdit = menu.addAction(QStringLiteral("채널 수정"));
            auto* actRetry = menu.addAction(QStringLiteral("재시도"));
            QMenu* swapMenu = menu.addMenu(QStringLiteral("위치 교환"));
            for (const auto& other : m_lastConfigs) {
                if (other.id == tile->channelId) continue;
                swapMenu->addAction(QString::fromStdString(other.name))->setData(
                    QString::fromStdString(other.id));
            }
            auto* actRemove = menu.addAction(QStringLiteral("채널 삭제"));
            auto* chosen = menu.exec(tile->mapToGlobal(p));
            if (chosen == actEdit) m_cb.editRequested(tile->channelId);
            else if (chosen == actRetry) m_cb.retryRequested(tile->channelId);
            else if (chosen == actRemove) m_cb.removeRequested(tile->channelId);
            else if (chosen != nullptr && !chosen->data().isNull())
                m_cb.swapRequested(tile->channelId, chosen->data().toString().toStdString());
        });
    }
    if (n == 0) {
        auto* empty = new QLabel(QStringLiteral("채널이 없습니다 — [채널 추가]를 누르세요"), this);
        empty->setAlignment(Qt::AlignCenter);
        m_grid->addWidget(empty, 0, 0);
    }
}

void GridView::updateTileStatus(const QString& channelId, const QString& state, int attempts,
                                const QList<int>& stages, double pps, qlonglong msSince) {
    auto it = m_tiles.find(channelId);
    if (it == m_tiles.end()) return;
    Tile* t = it->second;
    (void)stages;
    t->header->setText(QStringLiteral("%1  [%2%3]")
                           .arg(t->name, state,
                                attempts > 0 ? QStringLiteral(" %1회").arg(attempts) : QString()));
    if (msSince < 0) {
        t->flow->setText(QStringLiteral("패킷 —"));
        t->flow->setStyleSheet(QStringLiteral("color:gray; font-size:11px"));
    } else {
        t->flow->setText(QStringLiteral("패킷 %1/s · %2초 전")
                             .arg(pps, 0, 'f', 1)
                             .arg(msSince / 1000.0, 0, 'f', 1));
        const char* color = (msSince < 1000) ? "limegreen" : (msSince < 3000) ? "orange" : "red";
        t->flow->setStyleSheet(
            QStringLiteral("color:%1; font-weight:bold; font-size:11px").arg(color));
    }
}

} // namespace nv::ui
```

- [ ] **Step 4: `src/ui/channels/ChannelDialog.h/.cpp`** — 이름/URL 입력 모달:

```cpp
// ChannelDialog.h
#pragma once
#include <QDialog>
class QLineEdit;

namespace nv::ui {

class ChannelDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChannelDialog(QWidget* parent = nullptr, const QString& name = {},
                           const QString& url = {});
    QString name() const;
    QString url() const;

private:
    QLineEdit* m_name = nullptr;
    QLineEdit* m_url = nullptr;
};

} // namespace nv::ui
```

```cpp
// ChannelDialog.cpp
#include "ChannelDialog.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>

namespace nv::ui {

ChannelDialog::ChannelDialog(QWidget* parent, const QString& name, const QString& url)
    : QDialog(parent) {
    setWindowTitle(name.isEmpty() ? QStringLiteral("채널 추가") : QStringLiteral("채널 수정"));
    auto* form = new QFormLayout(this);
    m_name = new QLineEdit(name, this);
    m_url = new QLineEdit(url.isEmpty() ? QStringLiteral("rtsp://") : url, this);
    form->addRow(QStringLiteral("이름"), m_name);
    form->addRow(QStringLiteral("RTSP URL"), m_url);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

QString ChannelDialog::name() const { return m_name->text().trimmed(); }
QString ChannelDialog::url() const { return m_url->text().trimmed(); }

} // namespace nv::ui
```

- [ ] **Step 5: MainWindow 개편** — 단일 채널 UI를 그리드+툴바로 교체. `src/ui/shell/MainWindow.h`:

```cpp
#pragma once
#include <QComboBox>
#include <QMainWindow>
#include <functional>
#include "src/domain/channel/ChannelConfig.h"

namespace nv::ui {
class GridView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    struct Commands {
        std::function<void(std::string name, std::string url)> addChannel;
        std::function<void(std::string id, std::string name, std::string url)> updateChannel;
        std::function<void(std::string id)> removeChannel;
        std::function<void(std::string id)> retryChannel;
        std::function<void(std::string id)> framePainted;
        std::function<void(std::string a, std::string b)> swapChannels;
        // UI가 현재 채널 설정을 요청 (control 스레드 왕복 없이 마지막 캐시 사용)
    };

    MainWindow(GridView* grid, Commands commands);

    int manualColumns() const;            // 0 = Auto

public slots:
    // control 스레드 → queued. 목록 캐시 갱신 + 그리드 재구성
    void onChannelList(QVector<QString> ids, QVector<QString> names, QVector<QString> urls,
                       QVector<int> gridIndexes);
    void onSnapshot(QString channelId, QString state, int attempts, QList<int> stages,
                    double pps, qlonglong msSinceLastPacket);

private:
    void rebuildGrid();
    void openAddDialog();
    void openEditDialog(const std::string& id);

    Commands m_commands;
    GridView* m_grid = nullptr;
    QComboBox* m_columns = nullptr;
    std::vector<nv::domain::ChannelConfig> m_channels;   // UI 캐시
};

} // namespace nv::ui
```

`MainWindow.cpp` 핵심 (전체 구현):

```cpp
#include "MainWindow.h"
#include <QToolBar>
#include "src/ui/channels/ChannelDialog.h"
#include "src/ui/grid/GridView.h"

namespace nv::ui {

MainWindow::MainWindow(GridView* grid, Commands commands)
    : m_commands(std::move(commands)), m_grid(grid) {
    auto* tb = addToolBar(QStringLiteral("main"));
    tb->setMovable(false);
    auto* actAdd = tb->addAction(QStringLiteral("채널 추가"));
    connect(actAdd, &QAction::triggered, this, [this] { openAddDialog(); });

    m_columns = new QComboBox(this);
    m_columns->addItem(QStringLiteral("Auto"), 0);
    for (int c = 1; c <= 5; ++c) m_columns->addItem(QString::number(c), c);
    connect(m_columns, &QComboBox::currentIndexChanged, this, [this] { rebuildGrid(); });
    tb->addWidget(m_columns);

    setCentralWidget(m_grid);
    setWindowTitle(QStringLiteral("new_viewer M2a"));
    resize(1280, 800);
}

int MainWindow::manualColumns() const { return m_columns->currentData().toInt(); }

void MainWindow::onChannelList(QVector<QString> ids, QVector<QString> names,
                               QVector<QString> urls, QVector<int> gridIndexes) {
    m_channels.clear();
    for (int i = 0; i < ids.size(); ++i) {
        m_channels.push_back({ids[i].toStdString(), names[i].toStdString(),
                              urls[i].toStdString(), gridIndexes[i]});
    }
    rebuildGrid();
}

void MainWindow::rebuildGrid() {
    m_grid->rebuild(m_channels, manualColumns());
}

void MainWindow::onSnapshot(QString channelId, QString state, int attempts, QList<int> stages,
                            double pps, qlonglong msSinceLastPacket) {
    m_grid->updateTileStatus(channelId, state, attempts, stages, pps, msSinceLastPacket);
}

void MainWindow::openAddDialog() {
    ChannelDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted && !dlg.url().isEmpty())
        m_commands.addChannel(dlg.name().toStdString(), dlg.url().toStdString());
}

void MainWindow::openEditDialog(const std::string& id) {
    for (auto& c : m_channels) {
        if (c.id != id) continue;
        ChannelDialog dlg(this, QString::fromStdString(c.name), QString::fromStdString(c.url));
        if (dlg.exec() == QDialog::Accepted)
            m_commands.updateChannel(id, dlg.name().toStdString(), dlg.url().toStdString());
        return;
    }
}

} // namespace nv::ui
```

(GridView Callbacks의 editRequested는 main.cpp 조립에서 `win.openEditDialog`로 — private이므로 Commands에 editRequested 콜백을 추가하거나 openEditDialog를 public으로. **openEditDialog를 public으로** 두는 쪽으로 구현하라.)

- [ ] **Step 6: `src/ui/main.cpp` 전체 교체 (조립 루트)**

```cpp
#include <QApplication>
#include <QDir>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>
#include "src/app/ChannelManager.h"
#include "src/infra/ffmpeg/ChannelSourceFactory.h"
#include "src/infra/persist/JsonChannelRepository.h"
#include "src/infra/system/ControlExecutor.h"
#include "src/infra/system/ProcessStats.h"
#include "src/infra/system/StderrLogger.h"
#include "src/infra/system/SteadyClock.h"
#include "src/ui/grid/GridView.h"
#include "src/ui/shell/ControlBridge.h"
#include "src/ui/shell/MainWindow.h"

using namespace std::chrono_literals;

namespace {
int g_sigFd[2] = {-1, -1};
void onUnixSignal(int) {
    const char b = 1;
    (void)::write(g_sigFd[0], &b, 1);
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // --- infra ---
    nv::infra::SteadyClock clock;
    nv::infra::StderrLogger logger;
    const QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(cfgDir);
    nv::infra::JsonChannelRepository repo((cfgDir + "/channels.json").toStdString());

    // --- control 스레드 + 채널 매니저 ---
    std::atomic<nv::app::ChannelManager*> mgrPtr{nullptr};
    nv::infra::ControlExecutor executor(1s, [&mgrPtr] {
        if (auto* m = mgrPtr.load()) m->tickAll();
    });
    nv::infra::ChannelSourceFactory factory(executor);
    nv::app::ChannelManager mgr{repo, factory, clock, logger,
                                nv::domain::ReconnectPolicy{}, nv::domain::StallPolicy{}};
    mgrPtr.store(&mgr);

    // --- UI ---
    nv::ui::ControlBridge bridge;

    nv::ui::GridView::Callbacks gridCb;
    nv::ui::MainWindow* winPtr = nullptr;
    gridCb.framePainted = [&](std::string id) {
        executor.post([&, id] {
            if (auto* c = mgr.controller(id)) c->onFramePresented();
        });
    };
    gridCb.retryRequested = [&](std::string id) {
        executor.post([&, id] {
            if (auto* c = mgr.controller(id)) c->retry();
        });
    };
    gridCb.removeRequested = [&](std::string id) {
        executor.post([&, id] { mgr.removeChannel(id); });
    };
    gridCb.swapRequested = [&](std::string a, std::string b) {
        executor.post([&, a, b] { mgr.swapGrid(a, b); });
    };
    gridCb.editRequested = [&](std::string id) {
        if (winPtr != nullptr) winPtr->openEditDialog(id);
    };
    auto* grid = new nv::ui::GridView(factory, gridCb);

    nv::ui::MainWindow win(grid, {
        .addChannel = [&](std::string n, std::string u) {
            executor.post([&, n = std::move(n), u = std::move(u)] {
                const auto id = mgr.addChannel(n, u);
                if (!id.empty()) {
                    if (auto* c = mgr.controller(id)) c->connect();
                }
            });
        },
        .updateChannel = [&](std::string id, std::string n, std::string u) {
            executor.post([&, id, n = std::move(n), u = std::move(u)] {
                mgr.updateChannel(id, n, u);
            });
        },
        .removeChannel = gridCb.removeRequested,
        .retryChannel = gridCb.retryRequested,
        .framePainted = gridCb.framePainted,
        .swapChannels = gridCb.swapRequested,
    });
    winPtr = &win;

    // --- control → UI 배선 ---
    // 채널 목록 변경: control 스레드에서 스냅샷 떠서 queued 전달
    QObject::connect(&bridge, &nv::ui::ControlBridge::snapshotChanged,
                     &win, &nv::ui::MainWindow::onSnapshot);
    auto pushList = [&] {
        const auto cfgs = mgr.configs();
        QVector<QString> ids, names, urls;
        QVector<int> gi;
        for (auto& c : cfgs) {
            ids.push_back(QString::fromStdString(c.id));
            names.push_back(QString::fromStdString(c.name));
            urls.push_back(QString::fromStdString(c.url));
            gi.push_back(c.gridIndex);
        }
        QMetaObject::invokeMethod(&win, "onChannelList", Qt::QueuedConnection,
                                  Q_ARG(QVector<QString>, ids), Q_ARG(QVector<QString>, names),
                                  Q_ARG(QVector<QString>, urls), Q_ARG(QVector<int>, gi));
    };
    mgr.setListChangedObserver(pushList);

    // 각 채널 컨트롤러의 스냅샷 옵저버: 채널 생성 시점에 걸어야 한다.
    // ChannelManager에 훅이 없으므로, 옵저버 설정을 makeEntry에 내장하는 대신
    // setControllerObserverInstaller를 사용한다 — Task 3 구현에 아래 API를 추가하라:
    //   void setSnapshotObserver(std::function<void(const std::string&, const ChannelSnapshot&)>);
    //   (makeEntry에서 각 ctrl->setObserver에 channelId를 바인딩해 위 콜백 호출)
    mgr.setSnapshotObserver([&bridge](const std::string& id, const nv::app::ChannelSnapshot& s) {
        bridge.publish(QString::fromStdString(id), s);
    });

    // --- 시그널/종료/복원 ---
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, g_sigFd);
    std::signal(SIGINT, onUnixSignal);
    std::signal(SIGTERM, onUnixSignal);
    QSocketNotifier sigNotifier(g_sigFd[1], QSocketNotifier::Read);
    QObject::connect(&sigNotifier, &QSocketNotifier::activated, &app, [] {
        char b;
        (void)::read(g_sigFd[1], &b, 1);
        QApplication::quit();
    });

    win.show();
    const bool autoConnect = QApplication::arguments().contains(QStringLiteral("--connect"));
    executor.post([&, autoConnect] { mgr.restore(autoConnect); });

    // --- 소크 통계 (전 채널 합산 fps는 M2b 측정 스크립트에서 — 여기선 RSS만 유지) ---
    QDir().mkpath(QStringLiteral("logs"));
    QTimer statsTimer;
    QObject::connect(&statsTimer, &QTimer::timeout, &win, [] {
        std::FILE* csv = std::fopen("logs/soak.csv", "a");
        if (csv != nullptr) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            std::fprintf(csv, "%lld,%.1f\n", static_cast<long long>(ms),
                         nv::infra::processRssMb());
            std::fclose(csv);
        }
    });
    statsTimer.start(60'000);

    const int rc = QApplication::exec();
    executor.post([&] { mgr.disconnectAll(); });
    executor.drain();
    mgrPtr.store(nullptr);
    return rc;
}
```

**중요 — Task 3 보충**: 위 조립이 요구하는 `ChannelManager::setSnapshotObserver(std::function<void(const std::string&, const ChannelSnapshot&)>)`를 Task 3 구현에 포함하라 (makeEntry에서 `ctrl->setObserver([cb, id](auto& s){ cb(id, s); })` 바인딩, 옵저버가 나중에 설정되면 기존 엔트리에도 재바인딩). Task 3 테스트에 케이스 1개 추가:

```cpp
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
```

`QVector<QString>`/`QVector<int>`의 queued 전달은 Qt6 기본 등록 메타타입으로 동작한다.

- [ ] **Step 7:** CMake nv_ui 소스 추가(GridView, ChannelDialog), 빌드 + 단위 전체 PASS + 앱 기동 스모크(3초 생존) → Commit: `feat(ui): 멀티채널 그리드 UI — 타일·채널 관리·컬럼 선택·다채널 스냅샷 라우팅`

---

### Task 7: 20채널 시뮬 스크립트 + 베이스라인 측정 + 수동 검수 체크리스트

**Files:** Create `ops/sim-20ch.sh`, `ops/measure-baseline.sh`, `docs/m2a-acceptance.md`

- [ ] **Step 1: `ops/sim-20ch.sh`**

```bash
#!/bin/bash
# 20채널 시뮬: MediaMTX + ffmpeg 퍼블리셔 20개 (H.264 640x480@30 — 기준 부하)
# 사용: ./ops/sim-20ch.sh [채널수=20]   / 종료: Ctrl+C (트랩으로 일괄 정리)
set -uo pipefail
cd "$(dirname "$0")/.."
N="${1:-20}"
PORT=18600
[ -x ops/mediamtx/mediamtx ] || ./ops/mediamtx/download.sh
[ -f tests/fixtures/h264.mkv ] || ./tests/fixtures/make-fixtures.sh

cat > /tmp/nv-sim20.yml <<EOF
rtspAddress: :$PORT
api: no
rtmp: no
hls: no
webrtc: no
srt: no
paths:
  all_others:
EOF
./ops/mediamtx/mediamtx /tmp/nv-sim20.yml > /tmp/nv-sim20.log 2>&1 &
MTX=$!
sleep 1

PIDS=()
for i in $(seq 1 "$N"); do
  ffmpeg -re -stream_loop -1 -i tests/fixtures/h264.mkv -c copy \
    -f rtsp -rtsp_transport tcp "rtsp://127.0.0.1:$PORT/sim$i" >/dev/null 2>&1 &
  PIDS+=($!)
done
echo "sim ready: rtsp://127.0.0.1:$PORT/sim1 ... sim$N"
trap 'kill "${PIDS[@]}" $MTX 2>/dev/null; exit 0' INT TERM
wait
```

- [ ] **Step 2: `ops/measure-baseline.sh`**

```bash
#!/bin/bash
# 20채널 SW 디코딩 베이스라인 측정 (M2b 성능 게이트의 비교 기준)
# 전제: sim-20ch.sh 가동 중 + viewer에 sim1~sim20 채널 등록·연결 완료 상태에서 실행
# 사용: ./ops/measure-baseline.sh [측정초=300]
set -uo pipefail
cd "$(dirname "$0")/.."
DUR="${1:-300}"
PID=$(pgrep -f "new_viewer" | head -1)
[ -n "$PID" ] || { echo "new_viewer 미실행"; exit 1; }
OUT="logs/baseline-sw-$(date +%Y%m%d-%H%M).csv"
echo "ts,cpu_pct,rss_mb" > "$OUT"
END=$((SECONDS + DUR))
while [ $SECONDS -lt $END ]; do
  LINE=$(ps -o %cpu=,rss= -p "$PID")
  echo "$(date +%s),$(echo "$LINE" | awk '{print $1","$2/1024}')" >> "$OUT"
  sleep 5
done
awk -F, 'NR>1{c+=$2; r+=$3; n++} END{printf "평균 CPU %.1f%%, 평균 RSS %.1fMB (n=%d)\n", c/n, r/n, n}' "$OUT"
echo "saved: $OUT"
```

- [ ] **Step 3: `docs/m2a-acceptance.md`** — 수동 검수 체크리스트:

```markdown
# M2a 수용 검수 체크리스트

전제: `./ops/sim-20ch.sh` 가동, viewer 실행.

## UI 패리티 (기존 viewer와 비교하며 확인)
- [ ] 앱명 "영상관리시스템" + 로고 아이콘 (창/독)
- [ ] 좌측 채널 패널: 목록·상태 요약·우클릭 메뉴·추가/삭제 버튼, 토글 동작
- [ ] 중앙 "전체" 탭 그리드 (검정 배경), 우측 패널: 설정(컬럼)/파일(자리표시)/로그 탭, 토글 동작
- [ ] 상태바: 채널 집계 + CPU/메모리 갱신
- [ ] 로그 탭에 상태 전이 실시간 표시 (최대 2000줄 롤링)

## 기능
- [ ] 채널 추가 20개 (sim1~sim20) — 전 타일 영상 + 채널별 패킷 표시 동작
- [ ] 33번째 추가 시도 → 거부됨 (소프트 한도 32; 성능 보장선은 20)
- [ ] Auto 컬럼: 1개=1열, 4개=2열, 9개=3열, 16개=4열, 20개=5열 / 수동 1~5열 전환
- [ ] 채널 수정(이름/URL) → 즉시 반영, 해당 채널만 재연결
- [ ] 채널 삭제 → 해당 타일만 제거, **다른 채널 영상 끊김 없음** (R2 채널 독립)
- [ ] 위치 교환 → 두 타일 자리만 바뀜
- [ ] 앱 재시작 → 채널 목록·배치 그대로 복원 (--connect 시 자동 연결)
- [ ] 퍼블리셔 1개 kill → 해당 채널만 재접속 사이클, 나머지 19개 무영향
- [ ] 앱 종료(창닫기/Ctrl+C/SIGTERM) 후 MediaMTX 로그에 세션 20개 전부 "torn down" (유령 0)

## 베이스라인 (M2b 게이트 기준값)
- [ ] `./ops/measure-baseline.sh 300` 실행 → 평균 CPU/RSS를 docs/m2a-acceptance.md 결과란에 기록
- 결과: CPU ___% / RSS ___MB / 타일 끊김 관찰 ___건 (2026-__-__ 기록)
```

- [ ] **Step 4:** `chmod +x ops/sim-20ch.sh ops/measure-baseline.sh` → Commit: `feat(ops): 20채널 시뮬·SW 베이스라인 측정·M2a 검수 체크리스트`

---

### Task 8: 마무리 — README 갱신 + 셀프 체크

- [ ] **Step 1:** README 현재 상태에 `- [x] M2a: 멀티채널 그리드 (SW 디코딩)` 추가, `- [ ] M2b: HW 디코딩 + GPU 렌더 + Windows + 성능 게이트` 추가
- [ ] **Step 2:** 클린 빌드 + 전체 테스트 (단위 + 통합 -L integration, NV_MEDIAMTX_BIN 설정) 전부 PASS
- [ ] **Step 3:** domain/app 순수성 재확인: `grep -rn "include <Q" src/domain src/app` → 0건
- [ ] **Step 4:** Commit: `docs: README — M2a 완료 상태`

---

## 완료 기준 (M2a Definition of Done)

1. 단위테스트 전체 PASS (기존 47 + 신규 ~20)
2. 20채널 시뮬에서 검수 체크리스트 전 항목 통과 — 특히 **채널 독립**(삭제·장애 시 타 채널 무영향)과 **유령 0**(전 채널 torn down)
3. 재시작 복원 동작
4. SW 베이스라인 수치가 docs/m2a-acceptance.md에 기록됨 (M2b 게이트의 비교 기준)
5. domain/app에 Qt(Core 포함)/FFmpeg 의존 0 유지 (infra의 Qt Core는 허용 — 본 플랜에서 명시적 결정)

## M2b 미리보기 (이 플랜 범위 아님 — M2a 완료 후 별도 플랜)

HW 디코딩(VideoToolbox→D3D11VA), QRhiWidget GPU 렌더(카피 4→1회), disconnect 시 슬롯 클리어(tech-debt #4), Windows 빌드 브링업(vcpkg FFmpeg 동적/GPL제외), 성능 게이트(20ch CPU/드롭 — M2a 베이스라인 대비), macOS·Windows 양 플랫폼 검증.
