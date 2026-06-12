#include <catch2/catch_test_macros.hpp>
#include "src/ui/render/RendererSelect.h"

using nv::ui::RendererKind;
using nv::ui::selectRendererKind;

TEST_CASE("RHI 사용 가능하면 GPU 렌더러 선택") {
    REQUIRE(selectRendererKind(/*rhiAvailable=*/true) == RendererKind::Rhi);
}

TEST_CASE("RHI 불가하면 SW 폴백 선택 (GPU 없는 환경)") {
    REQUIRE(selectRendererKind(/*rhiAvailable=*/false) == RendererKind::Sw);
}
