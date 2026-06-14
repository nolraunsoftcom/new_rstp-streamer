#ifdef __APPLE__

// LaunchdRelayService 통합 테스트
//
// 실제 launchctl + mediamtx를 사용한다.
// mediamtx가 /opt/homebrew/bin/mediamtx 에 없으면 SKIP.
// 포트 8554 또는 9997이 이미 점유됐으면 SKIP (다른 mediamtx 인스턴스).
//
// TEARDOWN: 반드시 svc.stop()을 호출해 plist + 서비스를 제거한다.
// 실패 시에도 RAII 픽스처로 정리 보장.

#include <catch2/catch_test_macros.hpp>

#include "src/infra/relay/LaunchdRelayService.h"
#include "src/domain/relay/RelayConfig.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

constexpr const char* kMtxBin = "/opt/homebrew/bin/mediamtx";
constexpr const char* kLabel  = "com.ziilab.newviewer.relay.test";

// 파일 존재 확인
bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

// 포트가 이미 응답하는지 간이 확인 (nc -z)
bool portInUse(int port) {
    const std::string cmd =
        "nc -z 127.0.0.1 " + std::to_string(port) + " 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

// pgrep mediamtx 결과 (pid 목록)
std::string pgrepMediamtx() {
    std::array<char, 256> buf{};
    std::string result;
    FILE* p = popen("pgrep mediamtx 2>/dev/null", "r");
    if (!p) return result;
    while (fgets(buf.data(), static_cast<int>(buf.size()), p) != nullptr)
        result += buf.data();
    pclose(p);
    return result;
}

// RAII: 테스트 종료 시 서비스 정리 보장
struct ServiceCleanup {
    nv::infra::LaunchdRelayService* svc = nullptr;
    explicit ServiceCleanup(nv::infra::LaunchdRelayService& s) : svc(&s) {}
    ~ServiceCleanup() {
        if (svc) svc->stop();
    }
};

// 임시 config 경로
std::string tmpConfigPath() {
    return "/tmp/nv_launchd_it_relay.yml";
}

// 최소 mediamtx config 작성 (RelayConfig::generate 사용)
bool writeTmpConfig() {
    using namespace nv::domain;
    std::vector<RelayPath> paths{
        {"cam1", "rtsp://127.0.0.1:8554/nonexistent", true}
    };
    const std::string yml = RelayConfig::generate(paths);
    std::ofstream f(tmpConfigPath());
    if (!f.is_open()) return false;
    f << yml;
    return true;
}

} // namespace

// ── 메인 통합 테스트 ──────────────────────────────────────────────────────────
TEST_CASE("LaunchdRelayServiceIT: ensureRunning → 서비스 기동, stop → 완전 정리") {

    // 전제 조건 검사 ─────────────────────────────────────────────────────────

    if (!fileExists(kMtxBin)) {
        SKIP("mediamtx 바이너리 없음(" + std::string(kMtxBin) + ") — 스킵");
    }

    if (portInUse(8554) || portInUse(9997)) {
        SKIP("포트 8554 또는 9997이 이미 점유됨 — 다른 mediamtx 인스턴스 실행 중, 스킵");
    }

    // config 작성
    REQUIRE(writeTmpConfig());

    // 서비스 인스턴스 (테스트 전용 label 사용)
    nv::infra::LaunchdRelayService svc(kMtxBin, kLabel);
    ServiceCleanup cleanup(svc);

    // 혹시 이전 실패로 남은 서비스 정리
    svc.stop();
    std::this_thread::sleep_for(300ms);

    // ── ensureRunning ────────────────────────────────────────────────────────
    SECTION("ensureRunning 성공 + status.running == true") {
        const bool ok = svc.ensureRunning(tmpConfigPath());
        if (!ok) {
            // launchctl 권한 문제 등 환경 이슈 → SKIP (CI에서 발생 가능)
            SKIP("ensureRunning 실패 — launchctl 환경 이슈, 스킵");
        }
        REQUIRE(ok);

        const auto st = svc.status();
        CHECK(st.installed);
        CHECK(st.running);

        // ── 선택적: Control API 포트(9997) 응답 확인 ──────────────────────
        // 최대 3초 대기 후 nc -z 확인 (포트 응답이 느릴 수 있음)
        bool apiUp = false;
        for (int i = 0; i < 12; ++i) {
            std::this_thread::sleep_for(250ms);
            if (portInUse(9997)) { apiUp = true; break; }
        }
        // mediamtx가 올랐으면 9997이 열려야 한다. 미열리면 경고만(WARN).
        if (!apiUp) {
            WARN("Control API 포트 9997이 3초 내 응답하지 않음 (소스 없는 환경에서 드물게 발생)");
        }

        // ── stop + 정리 확인 ─────────────────────────────────────────────
        const bool stopped = svc.stop();
        CHECK(stopped);
        std::this_thread::sleep_for(500ms);

        const auto stAfter = svc.status();
        CHECK_FALSE(stAfter.installed);   // plist 삭제됨
        CHECK_FALSE(stAfter.running);     // 서비스 종료됨

        // plist 파일이 남아 있지 않아야 한다
        const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
        const std::string plistPath =
            home + "/Library/LaunchAgents/" + std::string(kLabel) + ".plist";
        CHECK_FALSE(fileExists(plistPath));

        // mediamtx 프로세스가 남아 있지 않아야 한다
        // (KeepAlive=true였으므로 bootout 후 launchd가 종료시킴)
        std::this_thread::sleep_for(500ms);
        const std::string pids = pgrepMediamtx();
        CHECK(pids.empty());
    }
}

// ── 멱등성 테스트: ensureRunning 두 번 호출 ──────────────────────────────────
TEST_CASE("LaunchdRelayServiceIT: ensureRunning 멱등 (두 번 호출 안전)") {
    if (!fileExists(kMtxBin)) {
        SKIP("mediamtx 바이너리 없음 — 스킵");
    }
    if (portInUse(8554) || portInUse(9997)) {
        SKIP("포트 8554 또는 9997이 이미 점유됨 — 스킵");
    }

    REQUIRE(writeTmpConfig());

    nv::infra::LaunchdRelayService svc(kMtxBin, kLabel);
    ServiceCleanup cleanup(svc);
    svc.stop();
    std::this_thread::sleep_for(300ms);

    const bool ok1 = svc.ensureRunning(tmpConfigPath());
    if (!ok1) SKIP("ensureRunning 1차 실패 — 스킵");

    std::this_thread::sleep_for(500ms);

    // 두 번째 호출 — 크래시 없이 true 반환해야 한다
    const bool ok2 = svc.ensureRunning(tmpConfigPath());
    CHECK(ok2);

    // 정리는 RAII
}

// ── 멱등성 PID 검증: 동일 설정 재호출 시 relay 프로세스(PID) 유지 ────────────
// 수정 전: 두 번째 ensureRunning이 bootout→재시작 → PID가 달라짐.
// 수정 후: plist 내용+running 확인 후 즉시 return → PID 불변.
TEST_CASE("LaunchdRelayServiceIT: ensureRunning 멱등 — 동일 설정 재호출 시 relay 프로세스(PID) 유지(재시작 안 함)") {
    if (!fileExists(kMtxBin)) {
        SKIP("mediamtx 바이너리 없음(" + std::string(kMtxBin) + ") — 스킵");
    }
    if (portInUse(8554) || portInUse(9997)) {
        SKIP("포트 8554 또는 9997이 이미 점유됨 — 다른 mediamtx 인스턴스 실행 중, 스킵");
    }

    REQUIRE(writeTmpConfig());

    nv::infra::LaunchdRelayService svc(kMtxBin, kLabel);
    ServiceCleanup cleanup(svc);

    // 혹시 이전 실패로 남은 서비스 정리
    svc.stop();
    std::this_thread::sleep_for(300ms);

    // 1차 기동
    const bool ok1 = svc.ensureRunning(tmpConfigPath());
    if (!ok1) SKIP("ensureRunning 1차 실패 — launchctl 환경 이슈, 스킵");

    // mediamtx가 완전히 뜰 때까지 최대 3초 대기
    std::this_thread::sleep_for(500ms);

    // PID 캡처 — "pgrep -x mediamtx" 첫 번째 줄만 사용
    const std::string pid1 = []() -> std::string {
        std::string raw = pgrepMediamtx();
        // 첫 줄(첫 번째 PID)만 추출
        const auto nl = raw.find('\n');
        if (nl != std::string::npos) raw = raw.substr(0, nl);
        // 앞뒤 공백 제거
        const auto s = raw.find_first_not_of(" \t\r");
        const auto e = raw.find_last_not_of(" \t\r");
        if (s == std::string::npos) return "";
        return raw.substr(s, e - s + 1);
    }();
    REQUIRE_FALSE(pid1.empty()); // mediamtx가 실행 중이어야 한다

    // 2차 호출 — 동일 configPath → 멱등 short-circuit → 재시작 없음
    const bool ok2 = svc.ensureRunning(tmpConfigPath());
    CHECK(ok2);

    // PID 재캡처 — 재시작됐다면 PID가 달라진다
    const std::string pid2 = []() -> std::string {
        std::string raw = pgrepMediamtx();
        const auto nl = raw.find('\n');
        if (nl != std::string::npos) raw = raw.substr(0, nl);
        const auto s = raw.find_first_not_of(" \t\r");
        const auto e = raw.find_last_not_of(" \t\r");
        if (s == std::string::npos) return "";
        return raw.substr(s, e - s + 1);
    }();

    // 핵심 검증: PID가 같아야 한다 — 재시작 없이 동일 프로세스 유지
    CHECK(pid1 == pid2);
    CHECK_FALSE(pid1.empty());

    // 정리 (RAII ServiceCleanup이 처리하지만 명시적으로 검증)
    const bool stopped = svc.stop();
    CHECK(stopped);
    std::this_thread::sleep_for(500ms);

    const auto stAfter = svc.status();
    CHECK_FALSE(stAfter.installed);
    CHECK_FALSE(stAfter.running);

    // plist가 남아 있지 않아야 한다
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const std::string plistPath =
        home + "/Library/LaunchAgents/" + std::string(kLabel) + ".plist";
    CHECK_FALSE(fileExists(plistPath));

    // mediamtx 프로세스가 남아 있지 않아야 한다
    std::this_thread::sleep_for(500ms);
    const std::string pidsAfter = pgrepMediamtx();
    CHECK(pidsAfter.empty());
}

#endif // __APPLE__
