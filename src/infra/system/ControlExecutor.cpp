#include "ControlExecutor.h"

#include <cstdio>
#include <exception>

namespace nv::infra {

ControlExecutor::ControlExecutor(std::chrono::milliseconds tickInterval,
                                 std::function<void()> onTick)
    : m_tickInterval(tickInterval), m_onTick(std::move(onTick)),
      m_thread(&ControlExecutor::run, this) {}

ControlExecutor::~ControlExecutor() {
    {
        std::lock_guard lk(m_mu);
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
}

void ControlExecutor::post(std::function<void()> fn) {
    {
        std::lock_guard lk(m_mu);
        m_queue.push_back(std::move(fn));
    }
    m_cv.notify_all();
}

void ControlExecutor::drain() {
    std::unique_lock lk(m_mu);
    m_idleCv.wait(lk, [this] { return m_queue.empty() && !m_busy; });
}

void ControlExecutor::run() {
    auto nextTick = std::chrono::steady_clock::now() + m_tickInterval;
    std::unique_lock lk(m_mu);
    for (;;) {
        m_cv.wait_until(lk, nextTick, [this] { return m_stop || !m_queue.empty(); });

        // 큐 우선 처리 (stop 후에도 남은 작업은 모두 실행한다)
        while (!m_queue.empty()) {
            auto fn = std::move(m_queue.front());
            m_queue.pop_front();
            m_busy = true;
            lk.unlock();
            // 작업 예외가 control 스레드 밖으로 새면 std::terminate(0xc0000409)로 앱 전체가
            // 죽는다. 한 작업 실패가 앱을 죽이지 않도록 경계에서 삼킨다(로깅 후 계속).
            try {
                fn();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[ControlExecutor] task threw: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "[ControlExecutor] task threw (unknown)\n");
            }
            lk.lock();
            m_busy = false;
        }
        m_idleCv.notify_all();

        if (m_stop) return;

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextTick) {
            lk.unlock();
            try {
                m_onTick();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[ControlExecutor] onTick threw: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "[ControlExecutor] onTick threw (unknown)\n");
            }
            lk.lock();
            nextTick = now + m_tickInterval;
        }
    }
}

} // namespace nv::infra
