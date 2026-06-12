#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QMetaObject>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>
#include "src/app/ChannelManager.h"
#include "src/infra/ffmpeg/ChannelSourceFactory.h"
#include "src/infra/persist/JsonChannelRepository.h"
#include "src/infra/system/CompositeLogger.h"
#include "src/infra/system/ControlExecutor.h"
#include "src/infra/system/ProcessStats.h"
#include "src/infra/system/SteadyClock.h"
#include "src/ui/channels/ChannelListPanel.h"
#include "src/ui/grid/GridView.h"
#include "src/ui/shell/ControlBridge.h"
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
    auto* grid = new nv::ui::GridView(&factory, gridCb);

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
    mgr.setListChangedObserver(pushList);

    mgr.setSnapshotObserver([&bridge](const std::string& id, const nv::app::ChannelSnapshot& s) {
        bridge.publish(QString::fromStdString(id), s);
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

    // --- 소크 통계 (60초마다 RSS 기록) ---
    QDir().mkpath(QStringLiteral("logs"));
    QTimer statsTimer;
    QObject::connect(&statsTimer, &QTimer::timeout, &win, [] {
        std::FILE* csv = std::fopen("logs/soak.csv", "a");
        if (csv != nullptr) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            std::fprintf(csv, "%lld,%.1f\n", static_cast<long long>(ms),
                         nv::infra::processRssMb());
            std::fclose(csv);
        }
    });
    statsTimer.start(60'000);

    const int rc = QApplication::exec();
    executor.post([&] { mgr.disconnectAll(); });
    executor.drain();
    mgrPtr.store(nullptr);
    return rc;
}
