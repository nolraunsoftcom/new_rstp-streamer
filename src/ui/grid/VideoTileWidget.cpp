#include "VideoTileWidget.h"
#include <cstdio>
#include <QVBoxLayout>
#include "src/app/ports/IVideoRenderer.h"
#include "src/ui/render/RhiVideoRenderer.h"
#include "src/ui/render/SwVideoRenderer.h"

namespace nv::ui {

VideoTileWidget::VideoTileWidget(nv::app::IFrameSurfaceRegistry& registry,
                                 std::string channelId,
                                 QWidget* parent)
    : QWidget(parent), m_registry(registry), m_channelId(std::move(channelId)) {
    setMinimumSize(320, 240);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    // 기본 GPU 경로. QRhi 초기화 실패 시 renderFailed → fallbackToSw().
    installRenderer(selectRendererKind(/*rhiAvailable=*/true));
}

void VideoTileWidget::installRenderer(RendererKind kind) {
    m_kind = kind;
    if (kind == RendererKind::Rhi) {
        // RHI 렌더러는 GpuTexture(NV12 zero-copy) 핸들의 수명을 직접 소유한다 — registry/
        // channelId를 주입해 in-flight 보관 후 releaseConsumed로 반납하게 한다(C3 계약).
        auto* rhi = new RhiVideoRenderer(this, &m_registry, m_channelId);
        m_renderer = rhi;
        connect(rhi, &RhiVideoRenderer::framePainted, this, &VideoTileWidget::framePainted);
        // QRhi 초기화/렌더 실패 시 런타임 SW 폴백.
        connect(rhi, &QRhiWidget::renderFailed, this, &VideoTileWidget::fallbackToSw);
    } else {
        auto* sw = new SwVideoRenderer(this);
        m_renderer = sw;
        connect(sw, &SwVideoRenderer::framePainted, this, &VideoTileWidget::framePainted);
    }
    m_layout->addWidget(m_renderer->widget());
}

void VideoTileWidget::fallbackToSw() {
    if (m_fellBack || m_kind == RendererKind::Sw) return;
    // GPU 렌더 경로 실패 — SW(QPainter)로 런타임 전환(영상은 계속 표시). 운영 가시성 1회 로그.
    std::fprintf(stderr, "[VideoTileWidget] render path = SW (QRhi fallback)\n");
    m_fellBack = true;
    // 기존 RHI 위젯 제거 후 SW 렌더러로 교체.
    QWidget* old = m_renderer->widget();
    m_layout->removeWidget(old);
    old->deleteLater();
    m_renderer = nullptr;
    m_seq = 0;   // 다음 폴링에서 최신 프레임을 SW로 다시 그리도록
    installRenderer(RendererKind::Sw);
}

void VideoTileWidget::pollFrame() {
    if (m_renderer == nullptr) return;
    // IFrameSurfaceRegistry 포트로 최신 서피스 조회 (CpuRgba / GpuTexture 모두).
    nv::app::FrameSurface surface;
    if (!m_registry.latestSurface(m_channelId, surface, m_seq)) return;
    m_seq = surface.seq;

    m_renderer->present(surface);

    // GpuTexture 핸들 수명(C3 계약):
    //  - RHI 렌더러: 핸들을 in-flight 보관 후 스스로 releaseConsumed로 반납한다 → 여기서 안 함.
    //  - SW 렌더러: gpuHandle을 무시(동반 RGBA만 그림)하므로 즉시 반납해야 누수가 없다.
    if (m_kind == RendererKind::Sw &&
        surface.kind == nv::app::FrameSurface::Kind::GpuTexture &&
        surface.gpuHandle != nullptr) {
        m_registry.releaseConsumed(m_channelId, surface.gpuHandle);
    }
}

} // namespace nv::ui
