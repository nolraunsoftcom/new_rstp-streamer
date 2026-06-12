#pragma once
#include <cstdint>
#include <vector>

namespace nv::app {

// 디코드 결과 1장. CPU 변종(RGBA tight)과 GPU 변종(플랫폼 텍스처 핸들)을 같은 타입으로 흐르게 한다.
// M2b: CPU 변종은 SW 폴백/테스트용, GPU 변종이 기본 경로.
struct FrameSurface {
    enum class Kind { None, CpuRgba, GpuTexture };
    Kind kind = Kind::None;
    int width = 0;
    int height = 0;
    uint64_t seq = 0;

    // CpuRgba일 때만 유효
    std::vector<uint8_t> rgba;          // tight, stride = width*4

    // GpuTexture일 때만 유효 — 불투명 핸들 (VideoToolbox: CVPixelBufferRef, D3D11: ID3D11Texture2D*).
    // 수명: 레지스트리가 ref를 보유, 소비자는 그릴 동안만 유효. void*로 두고 인프라 계층에서 캐스팅.
    void* gpuHandle = nullptr;
};

} // namespace nv::app
