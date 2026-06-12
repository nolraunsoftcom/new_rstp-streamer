#pragma once
#include <QImage>
#include <QWidget>
#include "src/app/ports/IVideoRenderer.h"

namespace nv::ui {

// CPU 폴백 렌더러 — QPainter+QImage로 RGBA 서피스를 그린다 (구 VideoTileWidget 로직 이관).
// CpuRgba/GpuTexture 서피스 모두 동반 RGBA(surface.rgba)를 그려 항상 화면이 나온다(안전 기본값).
// 새 seq를 실제로 그렸을 때 framePainted 발신 (= 도메인의 "표시 확정").
class SwVideoRenderer : public QWidget, public nv::app::IVideoRenderer {
    Q_OBJECT
public:
    explicit SwVideoRenderer(QWidget* parent = nullptr);

    void present(const nv::app::FrameSurface& surface) override;
    QWidget* widget() override { return this; }

signals:
    void framePainted();

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    QImage m_image;
    uint64_t m_seq = 0;
    bool m_paintedNew = false;
};

} // namespace nv::ui
