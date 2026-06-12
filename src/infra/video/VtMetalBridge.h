#pragma once

// VtMetalBridge — VideoToolbox CVPixelBuffer(NV12) → Metal 2-plane 텍스처(zero-copy).
//
// M2c Task2(인프라). CVMetalTextureCache로 디코드된 NV12 CVPixelBuffer의 두 평면을
//   plane 0: luma  Y    → MTLPixelFormatR8Unorm  (width × height)
//   plane 1: chroma CbCr → MTLPixelFormatRG8Unorm (width/2 × height/2)
// 두 장의 id<MTLTexture>로 만들어 셰이더(Task3)가 YUV→RGB 변환해 그리도록 한다.
// CPU 복사 0회 — 디코드 GPU 메모리를 그대로 샘플링.
//
// ── 플랫폼 ────────────────────────────────────────────────────────────────────
//   macOS(__APPLE__)에서만 실구현. 그 외 플랫폼은 전 메서드가 false/no-op 스텁.
//   선언은 플랫폼 중립(void* 핸들만 노출) — domain/app/ui로 Metal·CoreVideo 누출 0.
//
// ── 스레드/수명 계약 ─────────────────────────────────────────────────────────
//   * init/map/unmap/flush는 모두 렌더(UI) 스레드에서만 호출한다(QRhi와 동일 스레드).
//   * map()이 받는 cvPixelBuffer(CVPixelBufferRef)는 호출측(슬롯/렌더러)이 retain해
//     소유한다 — VtMetalBridge는 retain하지 않는다. map()이 만든 CVMetalTextureRef는
//     out.lumaRef/out.chromaRef에 담겨 반환되며, 가리키는 MTLTexture가 GPU에서
//     다 쓰일 때까지(프레임 in-flight) 살아 있어야 한다. 소비자는 그리기를 마친 뒤
//     unmap(out)으로 두 ref를 해제해야 한다(보통 더블버퍼: 직전 프레임 것을 unmap).

namespace nv::infra {

// map() 결과 — 두 평면 텍스처 + 수명 유지용 CVMetalTextureRef.
struct PlaneTextures {
    void* lumaTex = nullptr;     // id<MTLTexture>  — Y 평면 (R8Unorm)
    void* chromaTex = nullptr;   // id<MTLTexture>  — CbCr 평면 (RG8Unorm)
    void* lumaRef = nullptr;     // CVMetalTextureRef — luma 텍스처 수명 유지(소비 후 unmap이 release)
    void* chromaRef = nullptr;   // CVMetalTextureRef — chroma 텍스처 수명 유지
    int width = 0;               // luma(전체) 폭
    int height = 0;              // luma(전체) 높이
    bool fullRange = false;      // NV12 full-range(true) vs video-range(false) — 변환은 셰이더(Task3) 몫
};

class VtMetalBridge {
public:
    VtMetalBridge() = default;
    ~VtMetalBridge();

    VtMetalBridge(const VtMetalBridge&) = delete;
    VtMetalBridge& operator=(const VtMetalBridge&) = delete;

    // QRhi가 쓰는 동일 MTLDevice(id<MTLDevice>)로 CVMetalTextureCache 생성. 실패 시 false.
    bool init(void* mtlDevice);

    // cvPixelBuffer(CVPixelBufferRef)가 NV12면 두 평면을 텍스처화해 out 채우고 true.
    // NV12 아니거나 캐시 미초기화/생성 실패 시 false(out은 비변경 또는 부분 정리됨).
    bool map(void* cvPixelBuffer, PlaneTextures& out);

    // map()으로 받은 CVMetalTextureRef(lumaRef/chromaRef)를 release하고 포인터를 null로.
    void unmap(PlaneTextures& planes);

    // 캐시가 들고 있는 미사용 텍스처를 회수(프레임 끝/주기적으로 호출 권장).
    void flush();

private:
    void* m_cache = nullptr;     // CVMetalTextureCacheRef (CF 타입)
};

} // namespace nv::infra
