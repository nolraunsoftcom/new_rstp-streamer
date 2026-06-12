#include "SwVideoRenderer.h"
#include <QPainter>

namespace nv::ui {

SwVideoRenderer::SwVideoRenderer(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 120);
}

void SwVideoRenderer::present(const nv::app::FrameSurface& surface) {
    if (surface.seq == m_seq) return;                       // 동일 프레임 — 갱신 불필요
    if (surface.rgba.empty() || surface.width <= 0 || surface.height <= 0) return;
    m_seq = surface.seq;
    m_image = QImage(surface.rgba.data(), surface.width, surface.height,
                     surface.width * 4, QImage::Format_RGBA8888)
                  .copy();   // 슬롯/서피스 버퍼와 수명 분리
    m_paintedNew = true;
    update();
}

void SwVideoRenderer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    if (m_image.isNull()) {
        // 레거시 VlcWidget placeholder: #ededed 배경 + #777 14px "No Stream"
        p.fillRect(rect(), QColor(QStringLiteral("#ededed")));
        p.setPen(QColor(QStringLiteral("#777777")));
        QFont f = p.font();
        f.setPixelSize(14);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("No Stream"));
        return;
    }
    p.fillRect(rect(), Qt::black);
    const QSize scaled = m_image.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2),
                       scaled);
    p.drawImage(target, m_image);
    if (m_paintedNew) {
        m_paintedNew = false;
        emit framePainted();   // 표시 확정 — 새 seq를 그렸을 때만
    }
}

} // namespace nv::ui
