#include <catch2/catch_test_macros.hpp>
#include "src/infra/video/LatestFrameSlot.h"

using namespace nv::infra;

TEST_CASE("초기 상태: latest는 false") {
    LatestFrameSlot slot;
    LatestFrameSlot::Frame f;
    CHECK_FALSE(slot.latest(f, 0));
}

TEST_CASE("publish 후 latest: 새 seq면 true + 데이터 복사") {
    LatestFrameSlot slot;
    const uint8_t px[8] = {1, 2, 3, 4, 5, 6, 7, 8};   // 2x1 RGBA
    slot.publish(2, 1, px);
    LatestFrameSlot::Frame f;
    REQUIRE(slot.latest(f, 0));
    CHECK(f.width == 2);
    CHECK(f.height == 1);
    CHECK(f.seq == 1);
    CHECK(f.rgba == std::vector<uint8_t>(px, px + 8));
    CHECK_FALSE(slot.latest(f, f.seq));   // 같은 seq를 이미 봤으면 false
}

TEST_CASE("연속 publish는 마지막 것만 남는다 + seq 증가") {
    LatestFrameSlot slot;
    const uint8_t a[4] = {1, 1, 1, 1};
    const uint8_t b[4] = {2, 2, 2, 2};
    slot.publish(1, 1, a);
    slot.publish(1, 1, b);
    LatestFrameSlot::Frame f;
    REQUIRE(slot.latest(f, 0));
    CHECK(f.seq == 2);
    CHECK(f.rgba[0] == 2);
}
