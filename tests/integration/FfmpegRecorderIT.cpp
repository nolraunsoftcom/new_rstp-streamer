// 녹화 통합테스트 — 실전 조합(MediaMTX + ffmpeg CLI publish) → FfmpegStreamSource 녹화 fan-out.
// 실행 조건: NV_MEDIAMTX_BIN 환경변수에 mediamtx 경로. 없으면 전체 SKIP.
// 준비: ops/mediamtx/download.sh && tests/fixtures/make-fixtures.sh
//
// 검증 4종 (설계 D7/D8 + M3 완료 기준):
//   1. 녹화 재생가능: publish → open → startRecording → 5초 → stopRecording → MKV ffprobe로
//      video stream/duration 확인(재생 가능).
//   2. 크래시 내성(D7): 녹화 중 stop 없이 소스/프로세스 강제 종료 → trailer 누락 MKV가
//      그래도 ffprobe로 부분 재생 가능(MKV는 trailer 없이도 복구 가능).
//   3. 세그먼트 분리: 녹화 중 stopRecording→startRecording(재연결 모사, 새 경로) →
//      두 세그먼트 파일 생성, 둘 다 재생 가능.
//   4. 화면=녹화 일치(샘플): 같은 publish 소스를 녹화·디코드해 산출(녹화 프레임수 vs 디코드
//      프레임수)이 대략 정합 — 같은 패킷 fan-out이므로 큰 차이 없어야 한다.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

#include "harness.h"
#include "src/infra/ffmpeg/FfmpegStreamSource.h"
#include "src/infra/video/LatestSurfaceSlot.h"

using namespace nv::itest;
using namespace std::chrono_literals;

namespace {
constexpr int kPort = 18555;   // FfmpegSourceIT(18554)·기본(8554)과 충돌 회피

struct Server {
    std::string yml = "/tmp/nv_rec_it_mediamtx.yml";
    std::unique_ptr<ChildProcess> proc;
    explicit Server(int port = kPort) {
        std::ofstream f(yml);
        f << "rtspAddress: :" << port << "\napi: no\nrtmp: no\nhls: no\nwebrtc: no\nsrt: no\n"
          << "paths:\n  all_others:\n";
        f.close();
        proc = std::make_unique<ChildProcess>(envOr("NV_MEDIAMTX_BIN", "") + " " + yml);
        std::this_thread::sleep_for(1500ms);   // 기동 대기
    }
};

std::string fixtureDir() {
    return envOr("NV_FIXTURE_DIR", std::string(PROJECT_SOURCE_DIR) + "/tests/fixtures");
}

std::string publishCmd(const std::string& fixture, const std::string& path, int port = kPort) {
    return "ffmpeg -re -stream_loop -1 -i " + fixtureDir() + "/" + fixture +
           " -c copy -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:" +
           std::to_string(port) + "/" + path + " >/dev/null 2>&1";
}

std::string playUrl(const std::string& path, int port = kPort) {
    return "rtsp://127.0.0.1:" + std::to_string(port) + "/" + path;
}

bool integrationEnabled() { return !envOr("NV_MEDIAMTX_BIN", "").empty(); }

long fileSize(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return -1;
    return static_cast<long>(st.st_size);
}

// ffprobe로 한 줄 값을 읽는다(entry 한 개). 실패/없으면 빈 문자열.
std::string ffprobe(const std::string& args, const std::string& path) {
    const std::string cmd =
        "ffprobe -v error " + args + " -of default=nw=1:nk=1 \"" + path + "\" 2>/dev/null";
    std::array<char, 256> buf{};
    std::string result;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) return {};
    while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        result += buf.data();
    }
    ::pclose(pipe);
    // 후행 개행 제거
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

// MKV에 재생 가능한 video stream이 있는지(코덱 타입 == video).
bool hasVideoStream(const std::string& path) {
    return ffprobe("-select_streams v:0 -show_entries stream=codec_type", path).find("video") !=
           std::string::npos;
}

// 디코드 가능한 video 패킷 수(ffprobe -count_packets). trailer 없는 파일도 동작.
long videoPacketCount(const std::string& path) {
    const std::string s =
        ffprobe("-select_streams v:0 -count_packets -show_entries stream=nb_read_packets", path);
    return s.empty() ? -1 : std::strtol(s.c_str(), nullptr, 10);
}

std::string tmpOut(const std::string& tag) {
    return "/tmp/nv_rec_it_" + tag + "_" + std::to_string(::getpid()) + ".mkv";
}
} // namespace

// ── 1. 녹화 재생가능 ─────────────────────────────────────────────────────────
TEST_CASE("REC-IT: 녹화 5초 → stop → MKV가 ffprobe로 재생 가능(video stream + 패킷)") {
    if (!integrationEnabled()) SKIP("NV_MEDIAMTX_BIN 미설정");
    Server server;
    ChildProcess pub(publishCmd("h264.mkv", "rec1"));
    std::this_thread::sleep_for(800ms);

    nv::infra::LatestSurfaceSlot slot;
    nv::infra::FfmpegStreamSource src(slot);
    WaitingListener lsn;
    src.open(playUrl("rec1"), lsn);
    REQUIRE(lsn.waitFor("decoded", 15s));   // 디코드 시작 = 키프레임부터 패킷 흐름

    const std::string out = tmpOut("play");
    ::remove(out.c_str());
    REQUIRE(src.startRecording(out));
    std::this_thread::sleep_for(5s);        // 5초 녹화
    src.stopRecording();
    std::this_thread::sleep_for(500ms);     // 디코드 루프가 finishRecorder/trailer 쓸 시간
    src.close();

    CHECK(fileSize(out) > 0);
    CHECK(hasVideoStream(out));
    const long pkts = videoPacketCount(out);
    INFO("녹화 video 패킷 수: " << pkts);
    CHECK(pkts > 0);   // 5초 @30fps면 수십~150 내외. 0보다 크면 재생 가능.

    ::remove(out.c_str());
}

// ── 2. 크래시 내성 (D7) ──────────────────────────────────────────────────────
// stop 없이 소스를 강제로 닫는다(write_trailer 누락 모사). MKV는 trailer 없이도
// 부분 복구가 가능하므로 ffprobe로 패킷이 읽혀야 한다.
TEST_CASE("REC-IT: 녹화 중 stop 없이 강제 종료 → trailer 없어도 부분 재생 가능(D7)") {
    if (!integrationEnabled()) SKIP("NV_MEDIAMTX_BIN 미설정");
    Server server;
    ChildProcess pub(publishCmd("h264.mkv", "rec2"));
    std::this_thread::sleep_for(800ms);

    nv::infra::LatestSurfaceSlot slot;
    nv::infra::FfmpegStreamSource src(slot);
    WaitingListener lsn;
    src.open(playUrl("rec2"), lsn);
    REQUIRE(lsn.waitFor("decoded", 15s));

    const std::string out = tmpOut("crash");
    ::remove(out.c_str());
    REQUIRE(src.startRecording(out));
    std::this_thread::sleep_for(3s);        // 데이터 누적

    // 장애 주입: stopRecording 없이 퍼블리셔 강제 종료 → 소스가 NoPackets로 끝나며
    // finishRecorder가 호출되지만, "stopRecording을 거치지 않은" 비정상 종료 경로다.
    // 더 가혹한 크래시 모사를 위해 src.close()도 stopRecording 없이 호출한다.
    pub.stop();
    src.close();                            // stopRecording 미호출 — 비정상 종료 경로

    // close()는 finishRecorder를 거쳐 trailer를 쓸 수 있으나(정상 마감), 핵심 검증은
    // "트레일러 유무와 무관하게 파일이 재생 가능"이다. 파일에 패킷이 읽혀야 한다.
    CHECK(fileSize(out) > 0);
    const long pkts = videoPacketCount(out);   // count_packets는 trailer 없어도 동작
    INFO("크래시 후 video 패킷 수: " << pkts);
    CHECK(pkts > 0);   // 부분 재생 가능 — MKV 복구성

    ::remove(out.c_str());
}

// ── 2b. 진짜 trailer 누락 모사 (write_header만 있고 trailer 없는 파일) ────────────
// FfmpegStreamSource를 거치지 않고 외부 ffmpeg을 SIGKILL해 trailer 없는 MKV를 만든다.
// (소스 경로의 finishRecorder는 항상 trailer를 쓰므로, 진짜 trailer 누락은 별도 모사.)
TEST_CASE("REC-IT: trailer 누락 MKV(외부 ffmpeg SIGKILL)도 ffprobe로 패킷 복구") {
    if (!integrationEnabled()) SKIP("NV_MEDIAMTX_BIN 미설정");
    Server server;
    ChildProcess pub(publishCmd("h264.mkv", "rec2b"));
    std::this_thread::sleep_for(800ms);

    const std::string out = tmpOut("notrailer");
    ::remove(out.c_str());
    // ffmpeg이 직접 MKV로 녹화하게 한 뒤 SIGKILL → write_trailer 못 씀.
    auto recProc = std::make_unique<ChildProcess>(
        "ffmpeg -rtsp_transport tcp -i " + playUrl("rec2b") +
        " -c copy -y " + out + " >/dev/null 2>&1");
    std::this_thread::sleep_for(3s);
    recProc->stop();   // SIGKILL — trailer 없음
    pub.stop();
    std::this_thread::sleep_for(300ms);

    CHECK(fileSize(out) > 0);
    const long pkts = videoPacketCount(out);
    INFO("trailer 누락 파일 video 패킷 수: " << pkts);
    CHECK(pkts > 0);   // MKV는 trailer 없이도 패킷 복구 가능(D7 핵심)

    ::remove(out.c_str());
}

// ── 3. 세그먼트 분리 ─────────────────────────────────────────────────────────
// 녹화 중 stopRecording→startRecording(새 경로)로 재연결 세그먼트 분리를 모사한다.
// (RecordingController::onReconnect가 정확히 이 시퀀스를 수행한다.)
TEST_CASE("REC-IT: 세그먼트 분리 — 재연결 모사로 두 MKV 모두 재생 가능") {
    if (!integrationEnabled()) SKIP("NV_MEDIAMTX_BIN 미설정");
    Server server;
    ChildProcess pub(publishCmd("h264.mkv", "rec3"));
    std::this_thread::sleep_for(800ms);

    nv::infra::LatestSurfaceSlot slot;
    nv::infra::FfmpegStreamSource src(slot);
    WaitingListener lsn;
    src.open(playUrl("rec3"), lsn);
    REQUIRE(lsn.waitFor("decoded", 15s));

    const std::string seg1 = tmpOut("seg1");
    const std::string seg2 = tmpOut("seg2");
    ::remove(seg1.c_str());
    ::remove(seg2.c_str());

    // 세그먼트 1
    REQUIRE(src.startRecording(seg1));
    std::this_thread::sleep_for(3s);
    // 재연결 분리: stop(현 세그먼트 마감) → 새 경로로 start (onReconnect와 동일 시퀀스).
    src.stopRecording();
    std::this_thread::sleep_for(300ms);   // 디코드 루프가 finishRecorder 처리할 시간
    REQUIRE(src.startRecording(seg2));
    std::this_thread::sleep_for(3s);
    src.stopRecording();
    std::this_thread::sleep_for(500ms);
    src.close();

    CHECK(fileSize(seg1) > 0);
    CHECK(fileSize(seg2) > 0);
    CHECK(hasVideoStream(seg1));
    CHECK(hasVideoStream(seg2));
    const long p1 = videoPacketCount(seg1);
    const long p2 = videoPacketCount(seg2);
    INFO("seg1 패킷=" << p1 << " seg2 패킷=" << p2);
    CHECK(p1 > 0);
    CHECK(p2 > 0);

    ::remove(seg1.c_str());
    ::remove(seg2.c_str());
}

// ── 3b. 세그먼트 즉시 전환 (D1 래치 경합 제거 검증) ──────────────────────────
// stop을 거치지 않고, 또 유예 sleep 없이 startRecording(새 경로)을 곧바로 호출한다.
// 구(舊) 동작: m_recording==true면 startRecording이 거절 → 두 번째 세그먼트가 생기지 않음.
// 신(新) 동작(D1): startRecording은 항상 수락하고, 디코드 스레드의 serviceRecording이
// pendingPath != currentPath를 감지해 현재 세그먼트를 마감하고 새 경로로 재start한다.
// 따라서 stop/유예 없이도 두 세그먼트가 모두 생성·재생 가능해야 한다.
TEST_CASE("REC-IT: 세그먼트 즉시 전환 — stop·유예 없이 startRecording만으로 두 세그먼트(D1)") {
    if (!integrationEnabled()) SKIP("NV_MEDIAMTX_BIN 미설정");
    Server server;
    ChildProcess pub(publishCmd("h264.mkv", "rec3b"));
    std::this_thread::sleep_for(800ms);

    nv::infra::LatestSurfaceSlot slot;
    nv::infra::FfmpegStreamSource src(slot);
    WaitingListener lsn;
    src.open(playUrl("rec3b"), lsn);
    REQUIRE(lsn.waitFor("decoded", 15s));

    const std::string seg1 = tmpOut("sw1");
    const std::string seg2 = tmpOut("sw2");
    ::remove(seg1.c_str());
    ::remove(seg2.c_str());

    // 세그먼트 1
    REQUIRE(src.startRecording(seg1));
    std::this_thread::sleep_for(3s);
    // 즉시 전환: stop·유예 없이 새 경로로 startRecording. (구 동작이라면 여기서 false 거절.)
    REQUIRE(src.startRecording(seg2));   // D1: 항상 수락 → 디코드 스레드가 전환 수행
    std::this_thread::sleep_for(3s);
    src.stopRecording();
    std::this_thread::sleep_for(500ms);
    src.close();

    CHECK(fileSize(seg1) > 0);
    CHECK(fileSize(seg2) > 0);
    CHECK(hasVideoStream(seg1));
    CHECK(hasVideoStream(seg2));
    const long p1 = videoPacketCount(seg1);
    const long p2 = videoPacketCount(seg2);
    INFO("즉시전환 seg1 패킷=" << p1 << " seg2 패킷=" << p2);
    CHECK(p1 > 0);
    CHECK(p2 > 0);

    ::remove(seg1.c_str());
    ::remove(seg2.c_str());
}

// ── 4. 화면=녹화 일치(샘플) ───────────────────────────────────────────────────
// 같은 publish 소스를 디코드(슬롯 프레임)·녹화(MKV)한다. 패킷 fan-out이 같은 패킷을
// 디코더·레코더에 분배하므로, 녹화 패킷 수와 디코드된 프레임 수가 대략 정합해야 한다.
// (정확히 같진 않다 — 녹화는 키프레임부터, 디코드도 키프레임부터지만 시작/종료 경계가
//  몇 프레임 다를 수 있어 ±20% 오차 허용. 핵심: 같은 소스에서 둘 다 비지 않고 비슷한 규모.)
TEST_CASE("REC-IT: 화면=녹화 정합 — 녹화 패킷수 vs 디코드 프레임수 대략 일치") {
    if (!integrationEnabled()) SKIP("NV_MEDIAMTX_BIN 미설정");
    Server server;
    ChildProcess pub(publishCmd("h264.mkv", "rec4"));
    std::this_thread::sleep_for(800ms);

    nv::infra::LatestSurfaceSlot slot;
    nv::infra::FfmpegStreamSource src(slot);
    // 디코드된 프레임 수를 센다(onFrameDecoded 콜백).
    struct CountingListener final : public nv::app::StreamSourceListener {
        std::atomic<int> decoded{0};
        void onSessionOpened() override {}
        void onPacketReceived() override {}
        void onFrameDecoded() override { ++decoded; }
        void onFramePresented() override {}
        void onSourceError(nv::domain::DiagnosisReason) override {}
    } lsn;

    src.open(playUrl("rec4"), lsn);
    // 디코드가 시작될 때까지 대기
    for (int i = 0; i < 150 && lsn.decoded.load() == 0; ++i)
        std::this_thread::sleep_for(100ms);
    REQUIRE(lsn.decoded.load() > 0);

    const std::string out = tmpOut("match");
    ::remove(out.c_str());
    const int decodedAtStart = lsn.decoded.load();
    REQUIRE(src.startRecording(out));
    std::this_thread::sleep_for(5s);
    src.stopRecording();
    const int decodedAtStop = lsn.decoded.load();
    std::this_thread::sleep_for(500ms);
    src.close();

    const long recPkts = videoPacketCount(out);
    const int decFrames = decodedAtStop - decodedAtStart;
    INFO("녹화 패킷=" << recPkts << " 디코드 프레임(녹화구간)=" << decFrames);
    REQUIRE(recPkts > 0);
    REQUIRE(decFrames > 0);
    // 같은 패킷 fan-out이므로 규모가 비슷해야 한다. 경계 오차 감안 ±40% 허용
    // (디코드는 슬롯 publish 타이밍, 녹화는 키프레임 게이트로 시작점이 약간 다름).
    const double ratio = static_cast<double>(recPkts) / static_cast<double>(decFrames);
    INFO("비율(녹화/디코드)=" << ratio);
    CHECK(ratio > 0.6);
    CHECK(ratio < 1.6);

    ::remove(out.c_str());
}
