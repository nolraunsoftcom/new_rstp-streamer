// PngSnapshotWriter / RecordingPaths / ChannelSourceFactory 스냅샷 위임 단위테스트.
// 실제 PNG 파일을 생성해 헤더 매직과 비어있지 않음을 검증한다(Fake 슬롯 RGBA → PNG).
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "src/app/ports/IExecutor.h"
#include "src/infra/persist/PngSnapshotWriter.h"
#include "src/infra/persist/RecordingPaths.h"
#include "src/infra/ffmpeg/ChannelSourceFactory.h"
#include "src/infra/video/LatestSurfaceSlot.h"

using namespace nv::infra;

namespace {

// 즉시 실행 executor — open을 호출하지 않으므로 실제로는 거의 쓰이지 않지만
// ChannelSourceFactory 생성에 IExecutor& 가 필요하다.
struct InlineExecutor final : nv::app::IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

std::string tmpPng() {
    return std::string("/tmp/nv_snap_") + std::to_string(::getpid()) + ".png";
}

// 파일이 PNG 매직(\x89PNG)으로 시작하는지.
bool isPng(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    unsigned char sig[8] = {0};
    f.read(reinterpret_cast<char*>(sig), 8);
    return f.gcount() == 8 && sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G';
}

std::vector<uint8_t> solidRgba(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255;
    }
    return px;
}

} // namespace

TEST_CASE("PngSnapshotWriter: 유효 RGBA를 PNG로 저장(매직·비어있지 않음)") {
    const int w = 16, h = 8;
    const auto px = solidRgba(w, h, 200, 100, 50);
    const std::string out = tmpPng();

    REQUIRE(PngSnapshotWriter::write(out, w, h, px.data()));
    CHECK(isPng(out));

    std::ifstream f(out, std::ios::binary | std::ios::ate);
    CHECK(f.tellg() > 0);
    ::remove(out.c_str());
}

TEST_CASE("PngSnapshotWriter: 유효하지 않은 입력은 false") {
    const auto px = solidRgba(4, 4, 0, 0, 0);
    CHECK_FALSE(PngSnapshotWriter::write("/tmp/nv_should_not.png", 4, 4, nullptr));
    CHECK_FALSE(PngSnapshotWriter::write("/tmp/nv_should_not.png", 0, 4, px.data()));
    CHECK_FALSE(PngSnapshotWriter::write("/tmp/nv_should_not.png", 4, 0, px.data()));
}

TEST_CASE("RecordingPaths: 파일명 규칙(확장자·채널명 살균·타임스탬프)") {
    const std::string rec = RecordingPaths::recordingPath("front-door");
    const std::string snap = RecordingPaths::snapshotPath("front-door");
    CHECK(rec.size() > 4);
    CHECK(rec.substr(rec.size() - 4) == ".mkv");
    CHECK(snap.substr(snap.size() - 4) == ".png");
    CHECK(rec.find("front-door_") != std::string::npos);

    // 위험 문자(슬래시)는 '_'로 치환되어 경로 탈출이 없어야 한다.
    const std::string evil = RecordingPaths::snapshotPath("a/b:c*?");
    CHECK(evil.find("a/b:c") == std::string::npos);
    CHECK(evil.find("a_b_c") != std::string::npos);
}

TEST_CASE("ChannelSourceFactory: 슬롯 RGBA 있으면 snapshot true, 없으면 false") {
    InlineExecutor ex;
    ChannelSourceFactory factory(ex);

    const std::string out = tmpPng();

    // 슬롯/소스 미생성 — false.
    CHECK_FALSE(factory.snapshot("nochan", out));

    // 소스 생성 시 슬롯이 만들어진다(Bundle은 destroy 시까지 유지).
    auto src = factory.createSource("cam1");
    // 아직 프레임 발행 전 — RGBA 비어 false.
    CHECK_FALSE(factory.snapshot("cam1", out));

    // 슬롯에 RGBA 발행 후 — true + 실제 PNG.
    const int w = 8, h = 8;
    const auto px = solidRgba(w, h, 10, 20, 30);
    factory.slot("cam1")->publishCpu(w, h, px.data());
    CHECK(factory.snapshot("cam1", out));
    CHECK(isPng(out));

    ::remove(out.c_str());
}

TEST_CASE("ChannelSourceFactory: 녹화 위임 — 살아있는 소스 없으면 false") {
    InlineExecutor ex;
    ChannelSourceFactory factory(ex);

    // 소스 없음 — startRecording false, isRecording false, stop 무해.
    CHECK_FALSE(factory.startRecording("ghost", "/tmp/nv_ghost.mkv"));
    CHECK_FALSE(factory.isRecording("ghost"));
    factory.stopRecording("ghost");   // 크래시 없음

    // 소스 생성 후 — open 전이라 demux 스레드 없음. startRecording은 요청 래치만 세우고 true.
    auto src = factory.createSource("cam1");
    CHECK(factory.startRecording("cam1", RecordingPaths::recordingPath("cam1")));
    // 중복 요청은 false.
    CHECK_FALSE(factory.startRecording("cam1", RecordingPaths::recordingPath("cam1")));
    factory.stopRecording("cam1");
    CHECK_FALSE(factory.isRecording("cam1"));

    // 소스 파괴 후 위임 대상이 사라지면 false.
    src.reset();
    CHECK_FALSE(factory.startRecording("cam1", "/tmp/x.mkv"));
}
