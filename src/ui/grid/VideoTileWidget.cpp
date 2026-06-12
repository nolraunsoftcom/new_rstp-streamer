#include "VideoTileWidget.h"
#include <cstdio>
#include <QVBoxLayout>
#include "src/app/ports/IVideoRenderer.h"
#include "src/ui/render/RhiVideoRenderer.h"
#include "src/ui/render/SwVideoRenderer.h"

namespace nv::ui {

VideoTileWidget::VideoTileWidget(nv::infra::LatestSurfaceSlot& slot, QWidget* parent)
    : QWidget(parent), m_slot(slot) {
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
        auto* rhi = new RhiVideoRenderer(this);
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
    // RGBA 폴링 경로 사용 — GpuTexture 서피스도 B4가 동반 RGBA를 채우므로 그대로 그린다.
    // (GPU 핸들 직행/zero-copy는 후속 — 여기선 CVPixelBuffer ref 수명 관리가 필요 없어 누수 없음.)
    nv::infra::LatestSurfaceSlot::Frame f;
    if (!m_slot.latest(f, m_seq)) return;
    m_seq = f.seq;

    nv::app::FrameSurface surface;
    surface.kind = nv::app::FrameSurface::Kind::CpuRgba;
    surface.width = f.width;
    surface.height = f.height;
    surface.seq = f.seq;
    surface.rgba = std::move(f.rgba);
    m_renderer->present(surface);
}

} // namespace nv::ui
