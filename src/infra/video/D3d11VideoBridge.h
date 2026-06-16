#pragma once

// D3d11VideoBridge — D3D11VA 디코더 NV12 텍스처(배열) → RGBA 텍스처 GPU 변환 (Windows zero-CPU-copy).
//
// macOS VtMetalBridge의 Windows 대응. 단, D3D11에는 두 가지 추가 제약이 있어 "GPU 변환"(B안)을 쓴다:
//   1) 디코더 출력 텍스처는 D3D11_BIND_DECODER로 만들어져, SHADER_RESOURCE 바인드를 frames ctx에
//      추가하지 않으면 SRV를 못 만든다(HwContext가 추가).
//   2) Qt RHI의 createFrom은 NV12 멀티플레인/배열-슬라이스 SRV를 직접 만들지 못한다.
// 따라서 디코더 NV12 텍스처의 Y/UV 평면 SRV(배열 슬라이스 지정)를 직접 만들어 픽셀 셰이더로
// NV12→RGBA 변환해 일반 RGBA 텍스처를 만든다. 렌더러는 그 RGBA를 createFrom으로 래핑해 기존
// RGBA 파이프라인으로 그린다. CPU 왕복(av_hwframe_transfer_data + sws)을 제거한다.
//
// ── 디바이스/스레드 계약 ──────────────────────────────────────────────────────
//   * init(d3d11Device): QRhi가 쓰는 ID3D11Device(SharedGpuDevice로 공유). 디코더도 같은
//     디바이스를 써야 한다(디바이스 종속 텍스처). 다르면 init/convert가 실패 → CPU 폴백.
//   * convert/init/shutdown은 모두 렌더(UI) 스레드에서만 호출 — QRhi와 같은 immediate context를
//     쓰므로 같은 스레드에서 호출해야 상태 경합이 없다(present()에서 호출).
//   * out.tex는 브리지 소유. 다음 convert 또는 shutdown까지 유효. 소비자(렌더러)는 createFrom만
//     하고 소유하지 않는다.

namespace nv::infra {

// convert() 결과 — RGBA 텍스처(ID3D11Texture2D*).
struct RgbaTexture {
    void* tex = nullptr;   // ID3D11Texture2D* (DXGI_FORMAT_R8G8B8A8_UNORM), 공유 QRhi 디바이스 소유
    int width = 0;
    int height = 0;
};

class D3d11VideoBridge {
public:
    D3d11VideoBridge() = default;
    ~D3d11VideoBridge();

    D3d11VideoBridge(const D3d11VideoBridge&) = delete;
    D3d11VideoBridge& operator=(const D3d11VideoBridge&) = delete;

    // QRhi의 ID3D11Device로 셰이더/샘플러를 준비. 성공 시 true. 비-Windows는 항상 false.
    bool init(void* d3d11Device);

    // gpuFrame(AVFrame*)의 NV12 디코더 텍스처(data[0])·배열 슬라이스(data[1])를 width×height
    // RGBA로 GPU 변환. 성공 시 out 채우고 true. 실패(SRV/드라이버/크로스디바이스) 시 false →
    // 호출측 CPU 폴백. (AVFrame ref는 호출측이 보유 — 변환 동안 슬라이스 수명 유지.)
    bool convert(void* gpuFrame, int width, int height, RgbaTexture& out);

    // 모든 D3D11 자원 해제. 비-Windows는 no-op.
    void shutdown();

private:
    // 플랫폼 의존 자원은 .cpp(_WIN32)에서 불투명 포인터로 관리.
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace nv::infra
