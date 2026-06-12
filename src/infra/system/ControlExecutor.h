#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "src/app/ports/IExecutor.h"

namespace nv::infra {

// 전용 control 스레드: 직렬 작업 큐 + 주기 tick (설계 D6, 3차 리뷰).
class ControlExecutor final : public nv::app::IExecutor {
public:
    ControlExecutor(std::chrono::milliseconds tickInterval, std::function<void()> onTick);
    ~ControlExecutor() override;   // 남은 큐 실행 후 합류

    void post(std::function<void()> fn) override;
    void drain();                  // 현재 큐가 빌 때까지 호출자 블로킹 (테스트/종료용)

private:
    void run();

    std::chrono::milliseconds m_tickInterval;
    std::function<void()> m_onTick;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::condition_variable m_idleCv;
    std::deque<std::function<void()>> m_queue;
    bool m_stop = false;
    bool m_busy = false;
    std::thread m_thread;
};

} // namespace nv::infra
