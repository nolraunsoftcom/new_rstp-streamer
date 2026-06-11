#include <QApplication>
#include <chrono>
#include "src/app/ChannelController.h"
#include "src/app/MarshallingStreamSource.h"
#include "src/infra/ffmpeg/FfmpegStreamSource.h"
#include "src/infra/system/ControlExecutor.h"
#include "src/infra/system/StderrLogger.h"
#include "src/infra/system/SteadyClock.h"
#include "src/infra/video/LatestFrameSlot.h"
#include "src/ui/grid/VideoTileWidget.h"
#include "src/ui/shell/ControlBridge.h"
#include "src/ui/shell/MainWindow.h"

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // --- infra ---
    nv::infra::LatestFrameSlot frameSlot;
    nv::infra::FfmpegStreamSource ffmpegSource(frameSlot);
    nv::infra::SteadyClock clock;
    nv::infra::StderrLogger logger;

    // --- control 스레드 + 채널 ---
    // atomic: tick 스레드와 main의 설정/해제 간 데이터 레이스 방지
    std::atomic<nv::app::ChannelController*> ctrlPtr{nullptr};
    nv::infra::ControlExecutor executor(1s, [&ctrlPtr] {
        if (auto* c = ctrlPtr.load()) c->tick();   // tick은 control 스레드에서
    });
    nv::app::MarshallingStreamSource source(ffmpegSource, executor);
    nv::app::ChannelController ctrl{"ch1", "rtsp://169.254.4.1:8900/live",
                                    source, clock, logger,
                                    nv::domain::ReconnectPolicy{}, nv::domain::StallPolicy{}};
    ctrlPtr = &ctrl;

    // --- UI ---
    nv::ui::ControlBridge bridge;
    ctrl.setObserver([&bridge](const nv::app::ChannelSnapshot& s) { bridge.publish(s); });

    nv::ui::MainWindow win(frameSlot, {
        .connectTo = [&](std::string url) {
            executor.post([&, url = std::move(url)] {
                ctrl.setUrl(url);   // Idle일 때만 반영
                ctrl.connect();
            });
        },
        .disconnect = [&] { executor.post([&] { ctrl.disconnect(); }); },
        .retry = [&] { executor.post([&] { ctrl.retry(); }); },
    });
    QObject::connect(&bridge, &nv::ui::ControlBridge::snapshotChanged,
                     &win, &nv::ui::MainWindow::onSnapshot);   // 스레드 경계 → 자동 queued
    QObject::connect(win.tile(), &nv::ui::VideoTileWidget::framePainted, &win,
                     [&] { executor.post([&] { ctrl.onFramePresented(); }); });

    win.show();
    const int rc = QApplication::exec();

    // 종료 순서: UI 멈춤 → 채널 해제(소스 스레드 합류) → executor 정지
    executor.post([&] { ctrl.disconnect(); });
    executor.drain();
    ctrlPtr.store(nullptr);
    return rc;
}
