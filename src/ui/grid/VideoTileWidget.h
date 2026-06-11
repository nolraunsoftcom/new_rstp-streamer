#pragma once
#include <QImage>
#include <QTimer>
#include <QWidget>
#include "src/infra/video/LatestFrameSlot.h"

namespace nv::ui {

// 최신 프레임 슬롯을 30Hz 폴링해 그린다. 새 프레임을 실제로 그렸을 때 framePainted 발신
// (= 도메인의 "표시 확정", D2의 카운터 리셋 신호원).
class VideoTileWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoTileWidget(nv::infra::LatestFrameSlot& slot, QWidget* parent = nullptr);

signals:
    void framePainted();

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    void pollFrame();

    nv::infra::LatestFrameSlot& m_slot;
    QImage m_image;
    uint64_t m_seq = 0;
    bool m_paintedNew = false;
    QTimer m_timer;
};

} // namespace nv::ui
