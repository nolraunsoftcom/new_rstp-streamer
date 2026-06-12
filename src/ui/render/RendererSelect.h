#pragma once

namespace nv::ui {

// 렌더러 선택 정책 — Qt 비의존 순수 로직(GPU 없이 단위테스트 가능).
enum class RendererKind { Sw, Rhi };

// RHI 사용 가능하면 GPU(Rhi), 아니면 SW 폴백.
// 서피스 종류와 무관 — Rhi/Sw 렌더러 모두 동반 RGBA를 그린다(B4가 GpuTexture에도 RGBA 채움).
inline RendererKind selectRendererKind(bool rhiAvailable) {
    return rhiAvailable ? RendererKind::Rhi : RendererKind::Sw;
}

} // namespace nv::ui
