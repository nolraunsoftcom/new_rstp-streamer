#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QMetaObject>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <atomic>
#include <chrono>
#include <csignal>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include "src/app/ChannelManager.h"
#include "src/infra/ffmpeg/ChannelSourceFactory.h"
#include "src/infra/persist/JsonChannelRepository.h"
#include "src/infra/system/CompositeLogger.h"
#include "src/infra/system/ControlExecutor.h"
#include "src/infra/system/SoakLogger.h"
#include "src/infra/system/SteadyClock.h"
#include "src/infra/video/VtMetalBridge.h"  // M2c Task2 컴파일/링크 확인용(호출은 Task3). 헤더만 참조.
#include "src/ui/channels/ChannelListPanel.h"
#include "src/ui/grid/GridView.h"
#include "src/ui/shell/ControlBridge.h"
#include "src/ui/shell/RepaintClock.h"
#include "src/ui/shell/LogPanel.h"
#include "src/ui/shell/MainWindow.h"

using namespace std::chrono_literals;

namespace {
int g_sigFd[2] = {-1, -1};
void onUnixSignal(int) {
    const char b = 1;
    (void)::write(g_sigFd[0], &b, 1);
}
} // namespace

int main(int argc, char** argv) {
    // QRhiWidget(GPU 렌더러)가 동작하려면 최상위 윈도우가 QRhi로 컴포지팅(flush)해야 한다.
    // 이 플래그가 없으면 윈도우가 raster 컴포지팅으로 굳어 나중에 추가된 타일의 QRhiWidget이
    // QRhi를 못 받아 renderFailed → SW 폴백한다. QApplication 생성 전에 설정해야 적용된다.
    // (미설정 환경에서도 SW 폴백으로 안전하게 동작하지만, GPU 경로를 켜려면 필요.)
    if (!qEnvironmentVariableIsSet("QT_WIDGETS_RHI")) {
        qputenv("QT_WIDGETS_RHI", "1");
    }
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("영상관리시스템"));
    app.setWindowIcon(QIcon(QStringLiteral(":/logo.png")));

    // --- infra ---
    nv::infra::SteadyClock clock;
    nv::infra::CompositeLogger logger;
    const QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(cfgDir);
    nv::infra::JsonChannelRepository repo((cfgDir + QStringLiteral("/channels.json")).toStdString());

    // --- control 스레드 + 채널 매니저 ---
    std::atomic<nv::app::ChannelManager*> mgrPtr{nullptr};
    nv::infra::ControlExecutor executor(1s, [&mgrPtr] {
        if (auto* m = mgrPtr.load()) m->tickAll();
    });
    nv::infra::ChannelSourceFactory factory(executor);
    nv::app::ChannelManager mgr{repo, factory, clock, logger,
                                nv::domain::ReconnectPolicy{}, nv::domain::StallPolicy{}};
    mgrPtr.store(&mgr);

    // --- UI ---
    nv::ui::ControlBridge bridge;

    // LogPanel (로그 탭 배선 — CompositeLogger 콜백 → queued)
    auto* logPanel = new nv::ui::LogPanel();
    logger.setCallback([logPanel](const QString& text) {
        QMetaObject::invokeMethod(logPanel, "appendLine", Qt::QueuedConnection,
                                  Q_ARG(QString, text));
    });

    // 앱 전체 단일 repaint 타이머 — 타일별 30Hz 타이머(20ch=600Hz) 대신 1개
    nv::ui::RepaintClock repaintClock;

    // GridView callbacks
    nv::ui::MainWindow* winPtr = nullptr;
    nv::ui::GridView::Callbacks gridCb;
    gridCb.framePainted = [&](std::string id) {
        executor.post([&, id] {
            if (auto* c = mgr.controller(id)) c->onFramePresented();
        });
    };
    gridCb.retryRequested = [&](std::string id) {
        executor.post([&, id] {
            if (auto* c = mgr.controller(id)) c->retry();
        });
    };
    gridCb.removeRequested = [&](std::string id) {
        executor.post([&, id] { mgr.removeChannel(id); });
    };
    gridCb.swapRequested = [&](std::string a, std::string b) {
        executor.post([&, a, b] { mgr.swapGrid(a, b); });
    };
    gridCb.editRequested = [&](std::string id) {
        if (winPtr != nullptr) winPtr->openEditDialog(id);
    };
    auto* grid = new nv::ui::GridView(static_cast<nv::app::IFrameSurfaceRegistry*>(&factory), gridCb, repaintClock);

    // ChannelListPanel callbacks
    nv::ui::ChannelListPanel::Callbacks panelCb;
    panelCb.addRequested = [&] {
        if (winPtr != nullptr) winPtr->openEditDialog("");  // empty id = add
    };
    panelCb.editRequested = [&](std::string id) {
        if (winPtr != nullptr) winPtr->openEditDialog(id);
    };
    panelCb.removeRequested = gridCb.removeRequested;
    panelCb.retryRequested = gridCb.retryRequested;
    auto* channelPanel = new nv::ui::ChannelListPanel(panelCb);

    // MainWindow commands
    nv::ui::MainWindow::Commands winCmds;
    winCmds.addChannel = [&](std::string n, std::string u) {
        executor.post([&, n = std::move(n), u = std::move(u)] {
            const auto id = mgr.addChannel(n, u);
            if (!id.empty()) {
                if (auto* c = mgr.controller(id)) c->connect();
            }
        });
    };
    winCmds.updateChannel = [&](std::string id, std::string n, std::string u) {
        executor.post([&, id, n = std::move(n), u = std::move(u)] {
            mgr.updateChannel(id, n, u);
        });
    };
    winCmds.removeChannel = gridCb.removeRequested;
    winCmds.retryChannel = gridCb.retryRequested;
    winCmds.framePainted = gridCb.framePainted;
    winCmds.swapChannels = gridCb.swapRequested;

    nv::ui::MainWindow win(grid, channelPanel, logPanel, winCmds);
    winPtr = &win;

    // --- control → UI 배선 ---
    QObject::connect(&bridge, &nv::ui::ControlBridge::snapshotChanged,
                     &win, &nv::ui::MainWindow::onSnapshot);

    auto pushList = [&] {
        const auto cfgs = mgr.configs();
        QVector<QString> ids, names, urls;
        QVector<int> gi;
        for (const auto& c : cfgs) {
            ids.push_back(QString::fromStdString(c.id));
            names.push_back(QString::fromStdString(c.name));
            urls.push_back(QString::fromStdString(c.url));
            gi.push_back(c.gridIndex);
        }
        QMetaObject::invokeMethod(&win, "onChannelList", Qt::QueuedConnection,
                                  Q_ARG(QVector<QString>, ids),
                                  Q_ARG(QVector<QString>, names),
                                  Q_ARG(QVector<QString>, urls),
                                  Q_ARG(QVector<int>, gi));
    };

    // Fix 3: 옵저버 설정은 control 스레드(executor)에서만 — executor.post로 진입
    // restore post보다 먼저 post되도록 순서 유지 (직렬 보장)
    executor.post([&] {
        mgr.setListChangedObserver(pushList);
        mgr.setSnapshotObserver([&bridge](const std::string& id, const nv::app::ChannelSnapshot& s) {
            bridge.publish(QString::fromStdString(id), s);
        });
    });

    // --- 시그널/종료 ---
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, g_sigFd);
    std::signal(SIGINT, onUnixSignal);
    std::signal(SIGTERM, onUnixSignal);
    QSocketNotifier sigNotifier(g_sigFd[1], QSocketNotifier::Read);
    QObject::connect(&sigNotifier, &QSocketNotifier::activated, &app, [] {
        char b;
        (void)::read(g_sigFd[1], &b, 1);
        QApplication::quit();
    });

    win.show();
    const bool autoConnect = QApplication::arguments().contains(QStringLiteral("--connect"));
    executor.post([&, autoConnect] { mgr.restore(autoConnect); });

    // --- 소크 통계 (60초마다 RSS + 표시 fps 기록 — 4컬럼) ---
    const QString logsDir = cfgDir + QStringLiteral("/logs");
    QDir().mkpath(logsDir);
    nv::infra::SoakLogger soakLogger(factory, logsDir + QStringLiteral("/soak.csv"));
    soakLogger.start(60'000);

    const int rc = QApplication::exec();
    // Fix 4: 명시적 teardown — 콜백 해제 → 채널 정리 → 큐 비움 (스택 수명 의존 제거)
    executor.post([&] {
        mgr.setSnapshotObserver(nullptr);
        mgr.setListChangedObserver(nullptr);
        mgr.disconnectAll();
    });
    executor.drain();
    logger.setCallback(nullptr);
    mgrPtr.store(nullptr);
    return rc;
}
