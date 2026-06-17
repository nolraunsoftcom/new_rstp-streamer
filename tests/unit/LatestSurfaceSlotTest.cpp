#include <catch2/catch_test_macros.hpp>
#include "src/infra/video/LatestSurfaceSlot.h"

using namespace nv::infra;

// Fake GPU refcounter — retain/release 호출 횟수를 추적한다.
namespace {
struct FakeRefcounter {
    int retains = 0;
    int releases = 0;

    static void* retain(void* p) {
        auto* self = static_cast<FakeRefcounter*>(p);
        ++self->retains;
        return p;   // 편의상 자기 자신을 핸들로 반환
    }
    static void release(void* p) {
        auto* self = static_cast<FakeRefcounter*>(p);
        ++self->releases;
    }
};
} // namespace

TEST_CASE("clear() 후 latest(Frame)는 false를 반환한다") {
    LatestSurfaceSlot slot;
    const uint8_t px[4] = {1, 2, 3, 4};   // 1x1 RGBA
    slot.publishCpu(1, 1, px);

    LatestSurfaceSlot::Frame f;
    REQUIRE(slot.latest(f, 0));   // publish 직후에는 true

    slot.clear();

    // clear 후 seq==0이므로 latest는 항상 false (lastSeq=0이어도)
    CHECK_FALSE(slot.latest(f, 0));
}

TEST_CASE("clear() 후 재발행하면 정상적으로 latest가 true를 반환한다") {
    LatestSurfaceSlot slot;
    const uint8_t px[4] = {5, 6, 7, 8};
    slot.publishCpu(1, 1, px);

    slot.clear();

    // 재발행
    const uint8_t px2[4] = {9, 10, 11, 12};
    slot.publishCpu(1, 1, px2);

    LatestSurfaceSlot::Frame f;
    REQUIRE(slot.latest(f, 0));
    CHECK(f.rgba[0] == 9);
}

TEST_CASE("clear() 시 GPU refcounter release가 호출된다") {
    LatestSurfaceSlot slot;
    FakeRefcounter rc;

    slot.setGpuRefcounters(&FakeRefcounter::retain, &FakeRefcounter::release);

    // publishGpu: slot이 retain 1회
    const uint8_t px[4] = {1, 2, 3, 4};
    slot.publishGpu(1, 1, &rc, px);
    REQUIRE(rc.retains == 1);
    REQUIRE(rc.releases == 0);

    // clear(): 보유 핸들 release 1회
    slot.clear();
    CHECK(rc.releases == 1);

    // clear 후 latest는 false
    LatestSurfaceSlot::Frame f;
    CHECK_FALSE(slot.latest(f, 0));
}

TEST_CASE("destroySource 시뮬: clear 후 동일 채널 재생성 시 새 슬롯처럼 동작") {
    // ChannelSourceFactory의 동작을 슬롯 레벨에서 검증:
    // clear 후 재발행 → seq가 1부터 다시 올라가 소비자가 새 프레임으로 인식한다.
    LatestSurfaceSlot slot;
    const uint8_t a[4] = {1, 1, 1, 1};
    slot.publishCpu(1, 1, a);

    LatestSurfaceSlot::Frame f;
    REQUIRE(slot.latest(f, 0));
    const uint64_t seqBeforeClear = f.seq;

    slot.clear();
    CHECK_FALSE(slot.latest(f, 0));   // seq==0, lastSeq==0 → false

    // 동일 슬롯에 재발행
    const uint8_t b[4] = {2, 2, 2, 2};
    slot.publishCpu(1, 1, b);

    // lastSeq=0으로 조회하면 새 프레임 감지
    REQUIRE(slot.latest(f, 0));
    CHECK(f.rgba[0] == 2);
    // seq는 clear 이후 1부터 재시작 (이전 seqBeforeClear와 무관)
    (void)seqBeforeClear;
}

// ── jitter FIFO 동작 ────────────────────────────────────────────────────────────

TEST_CASE("FIFO: 표시 latest(FrameSurface)는 폴당 1프레임씩 순서대로 전진한다") {
    LatestSurfaceSlot slot;
    slot.setDepth(3);
    const uint8_t a[4] = {10, 0, 0, 0};
    const uint8_t b[4] = {20, 0, 0, 0};
    const uint8_t c[4] = {30, 0, 0, 0};
    slot.publishCpu(1, 1, a);
    slot.publishCpu(1, 1, b);
    slot.publishCpu(1, 1, c);

    nv::app::FrameSurface fs;
    uint64_t cursor = 0;
    REQUIRE(slot.latest(fs, cursor));   // 가장 오래된 것부터
    CHECK(fs.rgba[0] == 10);
    cursor = fs.seq;
    REQUIRE(slot.latest(fs, cursor));
    CHECK(fs.rgba[0] == 20);
    cursor = fs.seq;
    REQUIRE(slot.latest(fs, cursor));
    CHECK(fs.rgba[0] == 30);
    cursor = fs.seq;
    CHECK_FALSE(slot.latest(fs, cursor));   // 더 새 프레임 없음
}

TEST_CASE("FIFO: 스냅샷 latest(Frame)는 페이싱 없이 최신을 반환한다") {
    LatestSurfaceSlot slot;
    slot.setDepth(3);
    const uint8_t a[4] = {10, 0, 0, 0};
    const uint8_t b[4] = {20, 0, 0, 0};
    const uint8_t c[4] = {30, 0, 0, 0};
    slot.publishCpu(1, 1, a);
    slot.publishCpu(1, 1, b);
    slot.publishCpu(1, 1, c);

    LatestSurfaceSlot::Frame f;
    REQUIRE(slot.latest(f, 0));
    CHECK(f.rgba[0] == 30);   // 최신(큐 back)
}

TEST_CASE("FIFO: depth 초과 시 가장 오래된 것을 축출하고 GPU 핸들을 정확히 반납한다") {
    LatestSurfaceSlot slot;
    FakeRefcounter rc;
    slot.setGpuRefcounters(&FakeRefcounter::retain, &FakeRefcounter::release);
    slot.setDepth(2);

    const uint8_t px[4] = {1, 2, 3, 4};
    slot.publishGpu(1, 1, &rc, px);   // retain 1
    slot.publishGpu(1, 1, &rc, px);   // retain 2
    slot.publishGpu(1, 1, &rc, px);   // retain 3, 축출 1 → release 1
    slot.publishGpu(1, 1, &rc, px);   // retain 4, 축출 1 → release 2

    CHECK(rc.retains == 4);
    CHECK(rc.releases == 2);           // 큐엔 2개 남음(슬롯 소유 ref = 4-2 = 2)

    slot.clear();                      // 남은 2개 반납
    CHECK(rc.releases == 4);           // 완전 균형 (retain 4 == release 4)
}

TEST_CASE("FIFO: 멀티 리더가 각자 커서로 모든 프레임을 독립적으로 본다(비파괴)") {
    LatestSurfaceSlot slot;
    slot.setDepth(3);
    const uint8_t a[4] = {10, 0, 0, 0};
    const uint8_t b[4] = {20, 0, 0, 0};
    slot.publishCpu(1, 1, a);
    slot.publishCpu(1, 1, b);

    // 리더 A가 둘 다 소비
    nv::app::FrameSurface fa;
    uint64_t ca = 0;
    REQUIRE(slot.latest(fa, ca)); ca = fa.seq; CHECK(fa.rgba[0] == 10);
    REQUIRE(slot.latest(fa, ca)); ca = fa.seq; CHECK(fa.rgba[0] == 20);

    // 리더 B는 여전히 0부터 둘 다 볼 수 있어야 함(읽기는 비파괴)
    nv::app::FrameSurface fb;
    uint64_t cb = 0;
    REQUIRE(slot.latest(fb, cb)); cb = fb.seq; CHECK(fb.rgba[0] == 10);
    REQUIRE(slot.latest(fb, cb)); cb = fb.seq; CHECK(fb.rgba[0] == 20);
}

TEST_CASE("FIFO: latest(FrameSurface)는 소비자에게 추가 retain된 핸들을 넘긴다") {
    LatestSurfaceSlot slot;
    FakeRefcounter rc;
    slot.setGpuRefcounters(&FakeRefcounter::retain, &FakeRefcounter::release);
    slot.setDepth(2);

    const uint8_t px[4] = {1, 2, 3, 4};
    slot.publishGpu(1, 1, &rc, px);   // retain 1 (슬롯 소유)

    nv::app::FrameSurface fs;
    REQUIRE(slot.latest(fs, 0));
    CHECK(rc.retains == 2);            // 소비자 몫 추가 retain
    REQUIRE(fs.gpuHandle != nullptr);

    slot.releaseConsumed(fs.gpuHandle);
    CHECK(rc.releases == 1);           // 소비자 반납

    slot.clear();
    CHECK(rc.releases == 2);           // 슬롯 보유분 반납 → 균형
}
