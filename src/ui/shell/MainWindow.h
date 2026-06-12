#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <functional>
#include "src/infra/video/LatestFrameSlot.h"

namespace nv::ui {
class VideoTileWidget;

// 1채널 최소 셸. 비즈니스 로직 없음 — 명령은 콜백(=control 스레드 post)으로 위임.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    struct Commands {
        std::function<void(std::string)> connectTo;   // url
        std::function<void()> disconnect;
        std::function<void()> retry;
    };

    MainWindow(nv::infra::LatestFrameSlot& slot, Commands commands);
    VideoTileWidget* tile() { return m_tile; }

public slots:
    void onSnapshot(QString state, int attempts, QString reason, QList<int> stages,
                    double pps, qlonglong msSince);

private:
    Commands m_commands;
    VideoTileWidget* m_tile = nullptr;
    QLineEdit* m_url = nullptr;
    QLabel* m_flow = nullptr;
    QLabel* m_status = nullptr;
    QList<QLabel*> m_stageLabels;
};

} // namespace nv::ui
