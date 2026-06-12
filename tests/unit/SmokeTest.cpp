#include <catch2/catch_test_macros.hpp>
#include "tests/helpers/FakeClock.h"
#include "tests/helpers/FakeStreamSource.h"
#include "tests/helpers/FakeLogger.h"
#include "tests/helpers/FakeChannelRepository.h"
#include "tests/helpers/FakeRuntimeFactory.h"
#include "src/infra/system/SteadyClock.h"
#include "src/infra/system/ProcessStats.h"

TEST_CASE("smoke: 포트와 페이크가 컴파일된다") {
    nv::test::FakeClock clock;
    clock.advance(std::chrono::milliseconds(100));
    nv::test::FakeStreamSource source;
    nv::test::FakeLogger logger;
    CHECK(source.openCount == 0);
    CHECK(logger.entries.empty());
    CHECK(clock.now().time_since_epoch().count() > 0);

    nv::infra::SteadyClock steady;
    CHECK(steady.now().time_since_epoch().count() != 0);
    CHECK(nv::infra::processRssMb() > 0.0);

    nv::test::FakeChannelRepository repo;
    CHECK(repo.loadCount == 0);
    nv::test::FakeRuntimeFactory factory;
    CHECK(factory.createCount == 0);
}
