// FfmpegStreamSource 통합테스트 — 실전 조합(MediaMTX + ffmpeg CLI publish) 그대로.
// 실행 조건: NV_MEDIAMTX_BIN 환경변수에 mediamtx 경로. 없으면 전체 SKIP.
// 준비: ops/mediamtx/download.sh && tests/fixtures/make-fixtures.sh
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include "harness.h"
#include "src/infra/ffmpeg/FfmpegStreamSource.h"
#include "src/infra/video/LatestFrameSlot.h"

using namespace nv::itest;
using namespace std::chrono_literals;

namespace {
constexpr int kPort = 18554;   // 기본 8554와 충돌 회피

struct Server {
    std::string yml = "/tmp/nv_it_mediamtx.yml";
    std::unique_ptr<ChildProcess> proc;
    Server() {
        std::ofstream f(yml);
        f << "rtspAddress: :" << kPort << "\napi: no\nrtmp: no\nhls: no\nwebrtc: no\nsrt: no\n"
          << "paths:\n  all_others:\n";
        f.close();
        proc = std::make_unique<ChildProcess>(envOr("NV_MEDIAMTX_BIN", "") + " " + yml);
        std::this_thread::sleep_for(1500ms);   // 기동 대기
    }
};

std::string fixtureDir() {
    return envOr("NV_FIXTURE_DIR", std::string(PROJECT_SOURCE_DIR) + "/tests/fixtures");
}

std::string publishCmd(const std::string& fixture, const std::string& path) {
    return "ffmpeg -re -stream_loop -1 -i " + fixtureDir() + "/" + fixture +
           " -c copy -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:" +
           std::to_string(kPort) + "/" + path + " >/dev/null 2>&1";
}

std::string playUrl(const std::string& path) {
    return "rtsp://127.0.0.1:" + std::to_string(kPort) + "/" + path;
}

bool integrationEnabled() { return !envOr("NV_MEDIAMTX_BIN", "").empty(); }
} // namespace

TEST_CASE("IT: H.264 정상 재생 — 세션→패킷→디코딩→프레임 슬롯") {
    if (!integrationEnabled()) SKIP("NV_MEDIAMTX_BIN 미설정");
    Server server;
    ChildProcess pub(publishCmd("h264.mkv", "t264"));
    std::this_thread::sleep_for(800ms);   // publisher가 publish 상태가 될 때까지 대기

    nv::infra::LatestFrameSlot slot;
    nv::infra::FfmpegStreamSource src(slot);
    WaitingListener lsn;
    src.open(playUrl("t264"), lsn);

    CHECK(lsn.waitFor("session", 10s));
    CHECK(lsn.waitFor("packet", 5s));
    CHECK(lsn.waitFor("decoded", 15s));
    nv::infra::LatestFrameSlot::Frame f;
    // 프레임이 슬롯에 도달할 때까지 최대 5초 폴링
    bool got = false;
    for (int i = 0; i < 100 && !got; ++i) {
        got = slot.latest(f, 0);
        std::this_thread::sleep_for(100ms);
    }
    REQUIRE(got);
    CHECK(f.width == 640);
    CHECK(f.height == 480);
    src.close();
}

TEST_CASE("IT: H.265 정상 재생") {
    if (!integrationEnabled()) SKIP("NV_MEDIAMTX_BIN 미설정");
    Server server;
    ChildProcess pub(publishCmd("h265.mkv", "t265"));
    std::this_thread::sleep_for(800ms);   // publisher가 publish 상태가 될 때까지 대기

    nv::infra::LatestFrameSlot slot;
    nv::infra::FfmpegStreamSource src(slot);
    WaitingListener lsn;
    src.open(playUrl("t265"), lsn);

    CHECK(lsn.waitFor("session", 10s));
    CHECK(lsn.waitFor("decoded", 10s));
    src.close();
}

TEST_CASE("IT: 퍼블리셔 중단(무선 단절 모사) → NoPackets 에러") {
    if (!integrationEnabled()) SKIP("NV_MEDIAMTX_BIN 미설정");
    Server server;
    auto pub = std::make_unique<ChildProcess>(publishCmd("h264.mkv", "tkill"));
    std::this_thread::sleep_for(800ms);   // publisher가 publish 상태가 될 때까지 대기

    nv::infra::LatestFrameSlot slot;
    nv::infra::FfmpegStreamSource src(slot);
    WaitingListener lsn;
    src.open(playUrl("tkill"), lsn);
    REQUIRE(lsn.waitFor("decoded", 10s));

    pub->stop();                               // 장애 주입: 소스 사망
    CHECK(lsn.waitFor("error:NoPackets", 15s)); // 소켓 타임아웃(5s) 이내+여유
    src.close();
}

TEST_CASE("IT: 도달 불가 서버 → 즉시 에러 (행 금지)") {
    if (!integrationEnabled()) SKIP("NV_MEDIAMTX_BIN 미설정");
    nv::infra::LatestFrameSlot slot;
    nv::infra::FfmpegStreamSource src(slot);
    WaitingListener lsn;
    src.open("rtsp://127.0.0.1:1/none", lsn);  // 닫힌 포트
    CHECK(lsn.waitFor("error:", 10s));          // DeviceUnreachable 또는 SessionRefused
    src.close();
}

TEST_CASE("IT: close()는 TEARDOWN을 송신한다 (유령 세션 방지)") {
    if (!integrationEnabled()) SKIP("NV_MEDIAMTX_BIN 미설정");

    constexpr int kGtPort = 18556;
    std::string yml = "/tmp/nv_gt_mediamtx.yml";
    { std::ofstream f(yml);
      f << "logLevel: debug\nrtspAddress: :" << kGtPort << "\napi: no\nrtmp: no\nhls: no\nwebrtc: no\nsrt: no\npaths:\n  all_others:\n"; }
    ChildProcess mtx(envOr("NV_MEDIAMTX_BIN", "") + " " + yml + " > /tmp/nv_gt_mtx.log 2>&1");
    std::this_thread::sleep_for(700ms);

    ChildProcess pub("ffmpeg -re -stream_loop -1 -i " + fixtureDir() + "/h264.mkv"
                     " -c copy -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:" +
                     std::to_string(kGtPort) + "/gtd >/dev/null 2>&1");
    std::this_thread::sleep_for(800ms);

    nv::infra::LatestFrameSlot slot;
    nv::infra::FfmpegStreamSource src(slot);
    WaitingListener lsn;
    src.open("rtsp://127.0.0.1:" + std::to_string(kGtPort) + "/gtd", lsn);
    REQUIRE(lsn.waitFor("decoded", 10s));
    src.close();                                  // 스트리밍 중 종료 — TEARDOWN 나가야 함
    std::this_thread::sleep_for(500ms);

    std::ifstream log("/tmp/nv_gt_mtx.log");
    std::string all((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    // MediaMTX는 정상 TEARDOWN 시 "torn down by" 포함 라인을 남긴다
    const bool tornDown = all.find("torn down") != std::string::npos;
    INFO(all.substr(all.size() > 4000 ? all.size() - 4000 : 0));
    CHECK(tornDown);
}
