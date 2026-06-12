#pragma once
#include <QObject>
#include <QTimer>

namespace nv::ui {

// 앱 전체 단일 repaint 펄스 (~30Hz). 타일마다 타이머를 두지 않는다 (20ch×30Hz=600Hz 방지).
// M2b: 가능하면 QRhiWidget의 frameReady/vsync에 묶고, 폴백으로 QTimer 33ms.
class RepaintClock : public QObject {
    Q_OBJECT
public:
    explicit RepaintClock(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_timer, &QTimer::timeout, this, &RepaintClock::tick);
        m_timer.start(33);
    }
signals:
    void tick();
private:
    QTimer m_timer;
};

} // namespace nv::ui
