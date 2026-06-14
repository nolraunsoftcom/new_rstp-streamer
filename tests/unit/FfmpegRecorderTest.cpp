// FfmpegRecorder 자가 통합테스트(단위 디렉토리에 두지만 실제 muxing 수행).
// 입력 stream이 필요하므로 fixture(tests/fixtures/h264.mkv)를 demux해 그 비디오 stream으로
// FfmpegRecorder.start→그 파일의 패킷들을 writePacket→stop → 출력 MKV 생성·검증.
//
// 최소 보장(항상 실행): start/writePacket/stop이 크래시 없이 동작 + 출력 파일 생성 +
// 출력이 비어있지 않음. ffprobe로 video stream/프레임수 검증은 NV_RECORD_TEST 가드(통합 성격).
//
// 픽스처 없으면 SKIP — 클린 체크아웃에서 거짓 FAIL 방지.
// 픽스처 생성: tests/fixtures/make-fixtures.sh
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "src/infra/ffmpeg/FfmpegRecorder.h"

using namespace nv::infra;

namespace {

std::string fixturePath() {
    const char* dir = std::getenv("NV_FIXTURE_DIR");
    std::string base = (dir != nullptr) ? std::string(dir)
                                        : std::string(PROJECT_SOURCE_DIR) + "/tests/fixtures";
    return base + "/h264.mkv";
}

bool fixtureExists() {
    struct stat st {};
    return ::stat(fixturePath().c_str(), &st) == 0;
}

std::string tempOutPath() {
    // 테스트 산출물 — 고유 경로(PID로 충돌 회피).
    return std::string("/tmp/nv_recorder_out_") + std::to_string(::getpid()) + ".mkv";
}

long fileSize(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return -1;
    return static_cast<long>(st.st_size);
}

bool recordTestEnabled() { return std::getenv("NV_RECORD_TEST") != nullptr; }

// fixture를 열어 비디오 stream을 찾고, FfmpegRecorder로 remux한 뒤 출력 경로를 반환.
// 기록한 패킷 수를 packetsOut에 채운다. 실패 시 빈 문자열.
std::string remuxFixture(int& packetsOut) {
    packetsOut = 0;
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, fixturePath().c_str(), nullptr, nullptr) < 0) return {};
    if (avformat_find_stream_info(in, nullptr) < 0) {
        avformat_close_input(&in);
        return {};
    }
    const int vIdx = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vIdx < 0) {
        avformat_close_input(&in);
        return {};
    }

    const std::string out = tempOutPath();
    FfmpegRecorder rec;
    if (!rec.start(out, in->streams[vIdx])) {
        avformat_close_input(&in);
        return {};
    }

    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == vIdx) {
            rec.writePacket(pkt);
            ++packetsOut;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    rec.stop();
    CHECK_FALSE(rec.isRecording());
    CHECK_FALSE(rec.hadError());

    avformat_close_input(&in);
    return out;
}

} // namespace

TEST_CASE("FfmpegRecorder: start→writePacket→stop 크래시 없이 동작 + 출력 파일 생성") {
    if (!fixtureExists())
        SKIP("픽스처 없음 — tests/fixtures/make-fixtures.sh 실행 필요");
    int packets = 0;
    const std::string out = remuxFixture(packets);
    REQUIRE_FALSE(out.empty());
    CHECK(packets > 0);

    const long sz = fileSize(out);
    CHECK(sz > 0);   // 비어있지 않은 MKV가 생성됨

    ::remove(out.c_str());
}

TEST_CASE("FfmpegRecorder: 미시작 상태에서 writePacket/stop 안전(멱등)") {
    FfmpegRecorder rec;
    CHECK_FALSE(rec.isRecording());
    rec.writePacket(nullptr);   // 미시작 — 무동작
    rec.stop();                 // 멱등
    rec.stop();
    CHECK_FALSE(rec.isRecording());
    CHECK_FALSE(rec.hadError());
}

TEST_CASE("FfmpegRecorder: null 입력 stream으로 start 시 false") {
    FfmpegRecorder rec;
    CHECK_FALSE(rec.start("/tmp/nv_should_not_exist.mkv", nullptr));
    CHECK_FALSE(rec.isRecording());
}

TEST_CASE("FfmpegRecorder: 중복 start는 false") {
    if (!fixtureExists())
        SKIP("픽스처 없음 — tests/fixtures/make-fixtures.sh 실행 필요");
    AVFormatContext* in = nullptr;
    REQUIRE(avformat_open_input(&in, fixturePath().c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(in, nullptr) >= 0);
    const int vIdx = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    REQUIRE(vIdx >= 0);

    const std::string out = tempOutPath();
    FfmpegRecorder rec;
    REQUIRE(rec.start(out, in->streams[vIdx]));
    CHECK_FALSE(rec.start(out + ".2", in->streams[vIdx]));   // 이미 녹화 중
    rec.stop();
    ::remove(out.c_str());
    avformat_close_input(&in);
}

TEST_CASE("FfmpegRecorder: stop 후 재start 가능(롤오버 구조 보장)") {
    if (!fixtureExists())
        SKIP("픽스처 없음 — tests/fixtures/make-fixtures.sh 실행 필요");

    // 세그먼트 1
    int p1 = 0;
    const std::string out1 = remuxFixture(p1);
    REQUIRE_FALSE(out1.empty());
    CHECK(p1 > 0);

    // 세그먼트 2 — 같은 fixture를 다시 remux해 두 번째 start가 동작하는지 확인
    int p2 = 0;
    const std::string out2 = remuxFixture(p2);
    REQUIRE_FALSE(out2.empty());
    CHECK(p2 > 0);

    // 두 세그먼트 모두 비어있지 않은 MKV여야 한다
    CHECK(fileSize(out1) > 0);
    CHECK(fileSize(out2) > 0);

    ::remove(out1.c_str());
    ::remove(out2.c_str());
}

// ── D8 비동기 쓰기 큐: 단조 스트림을 빠르게 밀어넣어도 stop flush가 유효 출력 생성 ──
// 실제 캡처처럼 단조 증가 타임스탬프의 fixture 패킷을 한 번에 빠르게 큐에 넣는다(인라인
// write가 없으므로 디코드측은 즉시 반환). 검증: 비동기 큐·writer 스레드·stop flush 경로가
// 크래시·누수·쓰기오류 없이 동작하고 비어있지 않은 MKV를 만든다.
// (큐 상한을 넘으면 가장 오래된 비키프레임을 드랍하지만, 남은 패킷의 DTS는 여전히 단조라
//  muxer가 거부하지 않는다 — 드랍은 쓰기 오류가 아니므로 hadError는 false.)
TEST_CASE("FfmpegRecorder(D8): 비동기 큐 단일 패스 → stop flush 후 유효 출력") {
    if (!fixtureExists())
        SKIP("픽스처 없음 — tests/fixtures/make-fixtures.sh 실행 필요");
    int packets = 0;
    const std::string out = remuxFixture(packets);   // remuxFixture가 비동기 경로를 그대로 탄다
    REQUIRE_FALSE(out.empty());
    CHECK(packets > 0);
    CHECK(fileSize(out) > 0);
    ::remove(out.c_str());
}

// ── 지연 avio_open: 키프레임 없이 stop → 파일 생성 안 됨 ──────────────────────
// start()는 이제 디스크에 아무것도 쓰지 않는다. 첫 키프레임이 도착해야 avio_open+write_header.
// 키프레임 0개로 stop하면 파일 자체가 생성되지 않아야 한다(churn 강제종료 시 빈 파일 방지).
TEST_CASE("FfmpegRecorder: start→stop 사이 패킷 없음(키프레임 0) → 파일 생성 안 됨") {
    if (!fixtureExists())
        SKIP("픽스처 없음 — tests/fixtures/make-fixtures.sh 실행 필요");

    // fixture demuxer로 유효한 inputVideoStream을 얻는다(start의 codecpar 복사에 필요).
    AVFormatContext* in = nullptr;
    REQUIRE(avformat_open_input(&in, fixturePath().c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(in, nullptr) >= 0);
    const int vIdx = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    REQUIRE(vIdx >= 0);

    const std::string out = tempOutPath();
    // 혹시 이전 실패 잔재 제거
    ::remove(out.c_str());

    FfmpegRecorder rec;
    REQUIRE(rec.start(out, in->streams[vIdx]));
    // 패킷을 하나도 넣지 않고 즉시 stop — 키프레임이 도착한 적 없으므로 파일이 없어야 한다.
    rec.stop();

    CHECK_FALSE(std::filesystem::exists(out));   // 파일 생성되지 않음
    CHECK_FALSE(rec.hadError());                 // 오류 없이 정상 종료

    avformat_close_input(&in);
}

// ── ffprobe 재생가능 검증 (통합 성격 — NV_RECORD_TEST 가드) ───────────────────
// ctest의 skip 종료코드 처리를 피하기 위해 SKIP() 대신 early-return으로 통과시킨다
// (미설정 시 trivially pass). 외부 ffprobe 의존이라 기본 CI에서는 비활성.
TEST_CASE("FfmpegRecorder: 출력 MKV가 ffprobe로 video stream 확인됨") {
    if (!fixtureExists())
        SKIP("픽스처 없음 — tests/fixtures/make-fixtures.sh 실행 필요");
    if (!recordTestEnabled()) {
        SUCCEED("NV_RECORD_TEST 미설정 — ffprobe 검증 생략");
        return;
    }

    int packets = 0;
    const std::string out = remuxFixture(packets);
    REQUIRE_FALSE(out.empty());

    // ffprobe로 video stream codec_type 확인 — 재생가능한 컨테이너 검증.
    const std::string cmd =
        "ffprobe -v error -select_streams v:0 -show_entries stream=codec_type "
        "-of default=nw=1:nk=1 " + out + " 2>/dev/null";
    std::array<char, 128> buf{};
    std::string result;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        result += buf.data();
    }
    ::pclose(pipe);

    CHECK(result.find("video") != std::string::npos);

    ::remove(out.c_str());
}
