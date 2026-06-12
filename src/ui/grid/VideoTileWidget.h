#pragma once
#include <QImage>
#include <QWidget>
#include "src/infra/video/LatestSurfaceSlot.h"

namespace nv::ui {

// 최신 프레임 슬롯을 외부 tick 신호(RepaintClock::tick)로 폴링해 그린다.
// 자체 QTimer 없음 — 앱 전체 단일 RepaintClock이 ~30Hz 펄스를 발행한다.
// 새 프레임을 실제로 그렸을 때 framePainted 발신
// (= 도메인의 "표시 확정", D2의 카운터 리셋 신호원).
class VideoTileWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoTileWidget(nv::infra::LatestSurfaceSlot& slot, QWidget* parent = nullptr);

signals:
    void framePainted();

public slots:
    void pollFrame();

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    nv::infra::LatestSurfaceSlot& m_slot;
    QImage m_image;
    uint64_t m_seq = 0;
    bool m_paintedNew = false;
};

} // namespace nv::ui
