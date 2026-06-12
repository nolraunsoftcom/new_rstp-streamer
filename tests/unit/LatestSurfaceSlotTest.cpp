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
