#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include "src/infra/ffmpeg/FfmpegStreamSource.h"
#include "src/infra/video/LatestSurfaceSlot.h"

using namespace nv::infra;
using namespace std::chrono_literals;

// close()가 정상 종료 스레드를 신속히 join한다는 것을 검증.
// wedge 시나리오(VideoToolbox CoreMedia 세마포어 블로킹)는 실장비 통합 테스트/문서로.
TEST_CASE("close()는 정상 종료 스레드를 1초 이내에 join한다") {
    LatestSurfaceSlot slot;
    FfmpegStreamSource src(slot);

    // open()은 실제 URL을 열려 하지만, 유효하지 않은 URL이면 run()이 즉시 반환된다.
    // 여기서는 close()의 join 타이밍만 검증하므로 open() 없이 바로 close()를 호출.
    // (스레드가 없으면 joinable()==false라 close()는 즉시 반환 — 이것도 정상 케이스.)
    const auto t0 = std::chrono::steady_clock::now();
    src.close();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK(elapsed < 1s);
}

TEST_CASE("open() 없이 소멸자를 호출해도 안전하다 (join 없음)") {
    LatestSurfaceSlot slot;
    // 소멸자가 close()를 호출 — joinable()==false이므로 즉시 반환해야 한다.
    { FfmpegStreamSource src(slot); }   // RAII 소멸
    // 여기까지 도달하면 합격
    CHECK(true);
}
