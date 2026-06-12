#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include "src/app/ports/IStreamSource.h"

namespace nv::itest {

// 자식 프로세스 (mediamtx / ffmpeg publisher) — POSIX 전용 (macOS 벤치용. Windows는 CI 구성 시)
class ChildProcess {
public:
    // /bin/sh -c 로 실행. 프로세스 그룹으로 띄워 stopAll에서 일괄 종료.
    explicit ChildProcess(const std::string& cmd) {
        m_pid = fork();
        if (m_pid == 0) {
            setpgid(0, 0);
            execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
    }
    ~ChildProcess() { stop(); }
    void stop() {
        if (m_pid > 0) {
            kill(-m_pid, SIGKILL);
            int st = 0;
            waitpid(m_pid, &st, 0);
            m_pid = -1;
        }
    }
    bool running() const { return m_pid > 0; }

private:
    pid_t m_pid = -1;
};

// 이벤트 기록 + 대기 가능 리스너 (통합테스트는 어댑터 단독 검증이라 마샬링 불필요)
class WaitingListener final : public nv::app::StreamSourceListener {
public:
    void onSessionOpened() override { push("session"); }
    void onPacketReceived() override { push("packet"); }
    void onFrameDecoded() override { push("decoded"); }
    void onFramePresented() override { push("presented"); }
    void onSourceError(nv::domain::DiagnosisReason r) override {
        push(std::string("error:") + std::string(toString(r)));
    }

    // 해당 이벤트가 (이미 왔거나) timeout 안에 오면 true
    bool waitFor(const std::string& ev, std::chrono::milliseconds timeout) {
        std::unique_lock lk(m_mu);
        return m_cv.wait_for(lk, timeout, [&] {
            for (auto& e : m_events)
                if (e.rfind(ev, 0) == 0) return true;   // prefix 매치 (error:* 용)
            return false;
        });
    }

    std::vector<std::string> events() {
        std::lock_guard lk(m_mu);
        return m_events;
    }

private:
    void push(std::string e) {
        {
            std::lock_guard lk(m_mu);
            m_events.push_back(std::move(e));
        }
        m_cv.notify_all();
    }
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::vector<std::string> m_events;
};

inline std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : fallback;
}

} // namespace nv::itest
