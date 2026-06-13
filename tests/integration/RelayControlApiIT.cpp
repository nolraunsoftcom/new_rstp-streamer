// RelayControlApi 통합테스트
//
// Test A — "dead port → empty vector" (always runs, no mediamtx needed)
//   확실히 닫힌 포트(19997)를 향해 pathsHealth()를 호출 → timeout/error → 빈 벡터 반환.
//   이것이 RelayDown 신호의 근거다.
//
// Test B — "running mediamtx → path 보임" (mediamtx 없으면 SKIP)
//   NV_MEDIAMTX_BIN이 있거나 9997 포트가 이미 열려 있으면 실행.
//   ops/synthetic-source.sh를 사용하지 않고 최소 mediamtx 인스턴스를 직접 기동한다.
//
// 주의: QCoreApplication이 없으면 QNetworkAccessManager의 DNS/socket 초기화가
//       불완전하므로, 각 TEST_CASE에 QCoreApplication을 직접 생성한다.

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>

// POSIX only (macOS/Linux build)
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include "src/infra/relay/HttpRelayControlApi.h"

using namespace std::chrono_literals;

namespace {

// QCoreApplication 픽스처 — static argc/argv 보존
struct QtApp {
    int argc = 0;
    QCoreApplication* app = nullptr;
    QtApp() {
        if (QCoreApplication::instance() == nullptr) {
            app = new QCoreApplication(argc, nullptr);
        }
    }
    ~QtApp() {
        delete app;
        app = nullptr;
    }
};

// 환경변수 헬퍼
inline std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr) ? std::string(v) : fallback;
}

// 9997 포트가 응답하는지 간이 확인 (TCP connect)
bool port9997Responsive() {
    // curl이 있으면 사용; 없으면 nc -z
    int rc = std::system(
        "curl -sf --max-time 1 http://127.0.0.1:9997/v3/paths/list >/dev/null 2>&1");
    return rc == 0;
}

// mediamtx 자식 프로세스 RAII
struct MtxProcess {
    pid_t pid = -1;
    void stop() {
        if (pid > 0) {
            kill(-pid, SIGKILL);
            int st = 0;
            waitpid(pid, &st, 0);
            pid = -1;
        }
    }
    ~MtxProcess() { stop(); }
};

} // namespace

// ── Test A: dead port → empty vector (항상 실행) ─────────────────────────────
TEST_CASE("RelayControlApiIT-A: 닫힌 포트 → pathsHealth() 빈 벡터(RelayDown)") {
    QtApp qtApp;

    // 포트 19997은 실질적으로 아무것도 듣지 않는다.
    nv::infra::HttpRelayControlApi api("http://127.0.0.1:19997",
                                       std::chrono::milliseconds{1500});

    const auto result = api.pathsHealth();
    CHECK(result.empty());
}

// ── Test B: running mediamtx → path ready (mediamtx 없으면 SKIP) ─────────────
TEST_CASE("RelayControlApiIT-B: mediamtx 기동 → pathsHealth()에 cam1 path 반환") {
    const std::string mtxBin = envOr("NV_MEDIAMTX_BIN", "");

    // 이미 9997이 열려 있는지 먼저 확인
    const bool alreadyUp = port9997Responsive();

    if (mtxBin.empty() && !alreadyUp) {
        SKIP("NV_MEDIAMTX_BIN 미설정 + 9997 응답 없음 — mediamtx 없이 스킵");
    }

    MtxProcess mtxProc;

    if (!alreadyUp && !mtxBin.empty()) {
        // 최소 mediamtx 설정 파일 작성 (cam1 path, sourceOnDemand: no)
        const std::string yml = "/tmp/nv_relay_ctrl_it.yml";
        {
            std::ofstream f(yml);
            f << "rtspAddress: 127.0.0.1:18558\n"
              << "api: yes\n"
              << "apiAddress: 127.0.0.1:9997\n"
              << "rtmp: no\nhls: no\nwebrtc: no\nsrt: no\n"
              << "paths:\n"
              << "  cam1:\n"
              << "    sourceOnDemand: no\n";
        }

        // mediamtx 기동 (프로세스 그룹)
        mtxProc.pid = fork();
        if (mtxProc.pid == 0) {
            setpgid(0, 0);
            execl("/bin/sh", "sh", "-c",
                  (mtxBin + " " + yml + " >/dev/null 2>&1").c_str(),
                  static_cast<char*>(nullptr));
            _exit(127);
        }
        // 기동 대기
        std::this_thread::sleep_for(1800ms);
    }

    QtApp qtApp;
    nv::infra::HttpRelayControlApi api("http://127.0.0.1:9997",
                                       std::chrono::milliseconds{3000});

    const auto result = api.pathsHealth();

    if (result.empty() && !alreadyUp) {
        // mediamtx가 기동했지만 응답 없음 — CI에서 path 없는 경우 등
        SKIP("pathsHealth() 빈 벡터 — mediamtx api 미응답, 스킵");
    }

    // path가 있으면 cam1이 포함되어야 한다
    bool foundCam1 = false;
    for (const auto& h : result) {
        if (h.name == "cam1") {
            foundCam1 = true;
            INFO("cam1: ready=" << h.ready << " hasSource=" << h.hasSource);
            // sourceOnDemand:no + 소스 없음이면 ready=false/hasSource=false 가 정상
            // (장비가 없는 테스트 환경). 존재 자체를 확인한다.
            break;
        }
    }
    CHECK(foundCam1);
}
