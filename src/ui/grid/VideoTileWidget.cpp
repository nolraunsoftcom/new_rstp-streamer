#include "VideoTileWidget.h"
#include <QPainter>

namespace nv::ui {

VideoTileWidget::VideoTileWidget(nv::infra::LatestFrameSlot& slot, QWidget* parent)
    : QWidget(parent), m_slot(slot) {
    setMinimumSize(320, 240);
    // 자체 타이머 없음 — 외부 RepaintClock::tick에 pollFrame()을 연결해 사용한다.
}

void VideoTileWidget::pollFrame() {
    nv::infra::LatestFrameSlot::Frame f;
    if (!m_slot.latest(f, m_seq)) return;
    m_seq = f.seq;
    m_image = QImage(f.rgba.data(), f.width, f.height, f.width * 4,
                     QImage::Format_RGBA8888)
                  .copy();   // 슬롯 버퍼와 수명 분리
    m_paintedNew = true;
    update();
}

void VideoTileWidget::paintEvent(QPaintEvent*) {
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
