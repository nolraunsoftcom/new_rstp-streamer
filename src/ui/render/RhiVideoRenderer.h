#pragma once
#include <QImage>
#include <QRhiWidget>
#include <memory>
#include "src/app/ports/IVideoRenderer.h"

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiBuffer;
class QRhiTexture;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;
QT_END_NAMESPACE

namespace nv::ui {

// GPU 렌더러 — QRhiWidget(macOS=Metal). 서피스의 RGBA를 QRhi 텍스처로 업로드해
// 풀스크린 텍스처 사각형 셰이더로 화면에 그린다(KeepAspectRatio 레터박스).
//
// ── 경로 선택(태스크 가이드 따름) ─────────────────────────────────────────────
//   CVPixelBuffer→Metal 직행(zero-copy)은 CVMetalTextureCache 복잡도가 커서 후속으로 미룬다.
//   B4가 GpuTexture 서피스에도 동반 RGBA를 채우므로, 여기서는 그 RGBA를 GPU 텍스처로
//   업로드하는 더 단순·견고한 경로를 쓴다. 이미 디코드는 HW(VideoToolbox)이고,
//   스케일/컴포지팅이 GPU로 오프로딩되며 카피는 슬롯→QImage→텍스처 1회로 수렴한다.
//
// QRhi 초기화 실패 시 QRhiWidget이 renderFailed()를 발신 — 호스트(VideoTileWidget)가
// 이 신호를 받아 SW 렌더러로 런타임 폴백한다.
class RhiVideoRenderer : public QRhiWidget, public nv::app::IVideoRenderer {
    Q_OBJECT
public:
    explicit RhiVideoRenderer(QWidget* parent = nullptr);
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

    QRhi* m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;

    QImage m_pending;          // present()가 채운 최신 프레임 (UI 스레드)
    bool m_hasPending = false; // render()에서 업로드해야 할 새 프레임 존재
    uint64_t m_seq = 0;
    QSize m_texSize;           // 현재 텍스처 픽셀 크기 (변하면 재생성)
    bool m_vbufUploaded = false;
};

} // namespace nv::ui
