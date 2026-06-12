#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include "src/infra/system/ControlExecutor.h"

using namespace nv::infra;
using namespace std::chrono_literals;

TEST_CASE("post된 작업은 전부, 순서대로, 실행 스레드에서 실행된다") {
    std::vector<int> order;
    std::thread::id execThread;
    {
        ControlExecutor ex(1h, [] {});   // tick은 이 테스트와 무관하게 멀리
        ex.post([&] { order.push_back(1); execThread = std::this_thread::get_id(); });
        ex.post([&] { order.push_back(2); });
        ex.post([&] { order.push_back(3); });
        ex.drain();                       // 큐가 빌 때까지 대기 (테스트/종료용)
    }
    CHECK(order == std::vector<int>{1, 2, 3});
    CHECK(execThread != std::this_thread::get_id());
}

TEST_CASE("주기 tick 콜백이 실행 스레드에서 반복 호출된다") {
    std::atomic<int> ticks{0};
    {
        ControlExecutor ex(10ms, [&] { ++ticks; });
        std::this_thread::sleep_for(120ms);
    }
    CHECK(ticks.load() >= 3);    // 타이밍 여유: 12회 기대지만 3회만 보장 요구
}

TEST_CASE("소멸자는 남은 작업을 모두 실행하고 합류한다") {
    std::atomic<int> ran{0};
    {
        ControlExecutor ex(1h, [] {});
        for (int i = 0; i < 100; ++i) ex.post([&] { ++ran; });
    }
    CHECK(ran.load() == 100);
}
