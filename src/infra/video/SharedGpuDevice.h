#pragma once
#include <atomic>

namespace nv::infra {

// 프로세스 전역 GPU 디바이스 핸들 공유 — Windows D3D11 zero-copy/GPU변환 전용.
//
// D3D11 텍스처는 디바이스 종속이라, 디코더 출력 텍스처를 렌더 디바이스에서 바로
// 샘플링/변환하려면 둘이 같은 ID3D11Device를 써야 한다. 렌더러(QRhiWidget)가 초기화
// 시 QRhi의 ID3D11Device를 여기 등록하고, 디코드측 HwContext가 등록된 디바이스로
// D3D11VA hw 컨텍스트를 만든다. 미등록(렌더러 초기화 전 디코드 시작 등)이면 디코드측이
// 자체 디바이스로 폴백 → 크로스 디바이스라 GPU 변환 불가 → CPU 폴백(회귀 없음).
//
// 전제: 단일 최상위 창 앱이라 모든 QRhiWidget 타일이 창의 QRhi 디바이스 하나를 공유한다.
// macOS는 미사용 — VideoToolbox CVPixelBuffer(IOSurface)는 디바이스 무관이라 불필요.
class SharedGpuDevice {
public:
    static void setD3d11Device(void* dev) { s_d3d11.store(dev, std::memory_order_release); }
    static void* d3d11Device() { return s_d3d11.load(std::memory_order_acquire); }

private:
    inline static std::atomic<void*> s_d3d11{nullptr};
};

} // namespace nv::infra
