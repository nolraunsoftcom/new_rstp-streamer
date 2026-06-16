#pragma once
#include <QImage>
#include <QRhiWidget>
#include <cstdint>
#include <memory>
#include <string>
#include "src/app/ports/IVideoRenderer.h"
#include "src/infra/video/VtMetalBridge.h"
#include "src/infra/video/D3d11VideoBridge.h"

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiBuffer;
class QRhiTexture;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;
QT_END_NAMESPACE

namespace nv::app { class IFrameSurfaceRegistry; }

namespace nv::ui {

// GPU 렌더러 — QRhiWidget(macOS=Metal). 두 경로를 가진다:
//
//  1) NV12 zero-copy (기본, HW 디코드 + macOS): present()가 GpuTexture 서피스의
//     CVPixelBufferRef를 VtMetalBridge.map()으로 2-plane MTLTexture(Y=R8, CbCr=RG8)로
//     만들고 QRhiTexture::createFrom으로 래핑해 NV12→RGB 셰이더로 그린다. CPU 복사 0회.
//  2) RGBA 폴백: zero-copy 실패(비NV12/캐시 실패/createFrom 실패) 또는 CpuRgba 서피스 →
//     동반 RGBA를 QRhi 텍스처로 업로드해 그린다(기존 경로).
//
// ── CVPixelBuffer/MTLTexture 수명 (in-flight 더블버퍼) ───────────────────────────
//   LatestSurfaceSlot::latest()는 소비자에게 "추가 retain된" CVPixelBufferRef를 준다.
//   이 렌더러가 그 ref의 소유자다 — present()에서 받아 in-flight(GPU가 다 쓸 때까지) 보관,
//   다음 NV12 프레임이 들어와 교체될 때 직전 것을 unmap(CVMetalTextureRef release) +
//   registry.releaseConsumed(직전 CVPixelBufferRef)로 푼다(더블버퍼). 따라서 호스트
//   VideoTileWidget은 GpuTexture에 대해 releaseConsumed를 호출하지 않는다(이 렌더러가 책임).
//
// QRhi 초기화 실패 시 QRhiWidget이 renderFailed() 발신 — 호스트가 SW 렌더러로 폴백.
class RhiVideoRenderer : public QRhiWidget, public nv::app::IVideoRenderer {
    Q_OBJECT
public:
    // registry/channelId는 zero-copy 핸들 반납(releaseConsumed)용. nullptr이면(테스트 등)
    // GpuTexture 경로의 핸들 반납을 건너뛴다(슬롯 refcounter 미설정과 대칭).
    explicit RhiVideoRenderer(QWidget* parent = nullptr,
                              nv::app::IFrameSurfaceRegistry* registry = nullptr,
                              std::string channelId = {});
    ~RhiVideoRenderer() override;

    void present(const nv::app::FrameSurface& surface) override;
    QWidget* widget() override { return this; }

signals:
    void framePainted();

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    void releaseGpuResources();
    void ensureRgbaPipeline();
    void ensureNv12Pipeline();
    // 보관 중인 NV12 in-flight 자원(직전 프레임)을 해제: QRhiTexture 파괴 + bridge.unmap +
    // 슬롯에 CVPixelBufferRef 반납. handle 인자가 false면 releaseConsumed 생략(소멸자 경로 등).
    void releaseInflightNv12(bool returnHandle);

    QRhi* m_rhi = nullptr;

    // 공통: 풀스크린 쿼드 정점 + KeepAspectRatio scale 유니폼.
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiBuffer> m_ubuf;       // 정점단: vec2 scale (binding 0)
    std::unique_ptr<QRhiSampler> m_sampler;
    bool m_vbufUploaded = false;

    // ── RGBA 폴백 파이프라인 ─────────────────────────────────────────────────
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    QImage m_pending;            // present()가 채운 최신 RGBA 프레임
    bool m_hasPending = false;
    QSize m_texSize;             // RGBA 텍스처 픽셀 크기 (변하면 재생성)

    // ── NV12 zero-copy 파이프라인 ────────────────────────────────────────────
    nv::infra::VtMetalBridge m_bridge;
    bool m_bridgeReady = false;
    std::unique_ptr<QRhiBuffer> m_nv12Ubuf;   // 프래그먼트단: int fullRange (binding 3)
    std::unique_ptr<QRhiShaderResourceBindings> m_nv12Srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_nv12Pipeline;

    // NV12 present()가 준비한 "다음에 그릴" 자원 (UI 스레드, render()가 소비).
    bool m_hasNv12Pending = false;
    nv::infra::PlaneTextures m_pendingPlanes;
    std::unique_ptr<QRhiTexture> m_pendingLuma;
    std::unique_ptr<QRhiTexture> m_pendingChroma;
    void* m_pendingHandle = nullptr;     // 보류 프레임의 CVPixelBufferRef (소비자 소유 ref)
    int m_pendingFullRange = 0;
    QSize m_nv12Size;                     // 마지막으로 그린 NV12 luma 픽셀 크기

    // 현재 in-flight(그려진/그리는 중) NV12 자원 — 다음 교체 시 해제(더블버퍼).
    nv::infra::PlaneTextures m_inflightPlanes;
    std::unique_ptr<QRhiTexture> m_inflightLuma;
    std::unique_ptr<QRhiTexture> m_inflightChroma;
    void* m_inflightHandle = nullptr;

    // ── Windows D3D11 GPU 변환 경로 (NV12→RGBA, CPU 왕복 제거) ───────────────────
    // 디코더 NV12 텍스처를 공유 QRhi 디바이스에서 RGBA로 GPU 변환해 createFrom으로 래핑,
    // 기존 RGBA 파이프라인으로 그린다. macOS에선 m_d3dReady=false라 미사용.
#if defined(_WIN32)
    nv::infra::D3d11VideoBridge m_d3dBridge;
    bool  m_d3dReady = false;
    void* m_d3dRgbaTex = nullptr;            // 브리지 소유 RGBA D3D11 tex(non-owning, 최신)
    QSize m_d3dRgbaSize;
    bool  m_hasD3dPending = false;
    std::unique_ptr<QRhiTexture> m_d3dWrapTex;   // 위 RGBA를 래핑한 RHI 텍스처(createFrom)
    QSize m_d3dWrapSize;
#endif

    bool m_loggedPath = false;   // "render path = ..." 1회 마커
    uint64_t m_seq = 0;

    nv::app::IFrameSurfaceRegistry* m_registry = nullptr;
    std::string m_channelId;
};

} // namespace nv::ui
