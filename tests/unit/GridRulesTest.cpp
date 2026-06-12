#include <catch2/catch_test_macros.hpp>
#include "src/domain/layout/GridRules.h"

using nv::domain::grid::autoColumns;
using nv::domain::grid::rowsFor;

TEST_CASE("Auto 컬럼 규칙 (설계 §2)") {
    CHECK(autoColumns(0) == 3);
    CHECK(autoColumns(1) == 1);
    CHECK(autoColumns(2) == 2);
    CHECK(autoColumns(4) == 2);
    CHECK(autoColumns(5) == 3);
    CHECK(autoColumns(9) == 3);
    CHECK(autoColumns(10) == 4);
    CHECK(autoColumns(16) == 4);
    CHECK(autoColumns(17) == 5);
    CHECK(autoColumns(20) == 5);
}

TEST_CASE("행 수 계산") {
    CHECK(rowsFor(0, 3) == 1);    // 빈 그리드도 1행 (No Stream 표시 공간)
    CHECK(rowsFor(1, 1) == 1);
    CHECK(rowsFor(4, 2) == 2);
    CHECK(rowsFor(5, 3) == 2);
    CHECK(rowsFor(20, 5) == 4);
}
