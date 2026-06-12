#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QMetaObject>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <atomic>
#include <chrono>
#include <csignal>
#include <map>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include "src/app/ChannelManager.h"
#include "src/app/RecordingController.h"
#include "src/app/SnapshotService.h"
#include "src/domain/recording/RecordingState.h"
#include "src/infra/ffmpeg/ChannelSourceFactory.h"
#include "src/infra/persist/JsonChannelRepository.h"
#include "src/infra/persist/RecordingPaths.h"
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

    // M3-5: RecordingController + SnapshotService (control 스레드에서만 호출)
    // NV_RECORD_DIR을 RecordingPaths::baseDir()로 설정해 컨트롤러 경로와 일치
    const std::string recBaseDir = nv::infra::RecordingPaths::baseDir();
    if (std::getenv("NV_RECORD_DIR") == nullptr) {
        qputenv("NV_RECORD_DIR", QByteArray::fromStdString(recBaseDir));
    }
    nv::app::RecordingController recCtrl(factory, clock, logger);
    nv::app::SnapshotService snapSvc(factory, logger);

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
    // M3-5: 📷 스냅샷 버튼 — control 스레드로 post
    gridCb.snapshotRequested = [&](std::string id) {
        executor.post([&, id] {
            // 채널 이름 조회
            std::string name;
            for (const auto& c : mgr.configs()) {
                if (c.id == id) { name = c.name; break; }
            }
            const std::string path = nv::infra::RecordingPaths::snapshotPath(name);
            snapSvc.capture(id, name, path);
            // 스냅샷 후 파일 패널 갱신 (queued — UI 스레드)
            if (winPtr != nullptr) {
                QMetaObject::invokeMethod(winPtr, "onRecordingState",
                    Qt::QueuedConnection,
                    Q_ARG(QString, QString::fromStdString(id)),
                    Q_ARG(bool, recCtrl.stateOf(id) == nv::domain::RecordingState::Recording));
            }
        });
    };
    // M3-5: ● 녹화 토글 버튼 — control 스레드로 post
    gridCb.recordToggleRequested = [&](std::string id) {
        executor.post([&, id] {
            std::string name;
            for (const auto& c : mgr.configs()) {
                if (c.id == id) { name = c.name; break; }
            }
            recCtrl.toggle(id, name);
        });
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
    winCmds.addChannel = [&](std::string n, std::string u, bool ac) {
        executor.post([&, n = std::move(n), u = std::move(u), ac] {
            const auto id = mgr.addChannel(n, u, ac);
            if (!id.empty()) {
                if (auto* c = mgr.controller(id)) c->connect();
            }
        });
    };
    winCmds.updateChannel = [&](std::string id, std::string n, std::string u, bool ac) {
        executor.post([&, id, n = std::move(n), u = std::move(u), ac] {
            mgr.updateChannel(id, n, u, ac);
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
        QVector<bool> ac;
        for (const auto& c : cfgs) {
            ids.push_back(QString::fromStdString(c.id));
            names.push_back(QString::fromStdString(c.name));
            urls.push_back(QString::fromStdString(c.url));
            gi.push_back(c.gridIndex);
            ac.push_back(c.autoConnect);
        }
        QMetaObject::invokeMethod(&win, "onChannelList", Qt::QueuedConnection,
                                  Q_ARG(QVector<QString>, ids),
                                  Q_ARG(QVector<QString>, names),
                                  Q_ARG(QVector<QString>, urls),
                                  Q_ARG(QVector<int>, gi),
                                  Q_ARG(QVector<bool>, ac));
    };

    // M3-6: onReconnect 세그먼트 배선 — 채널이 재연결(끊김→재open)에 진입하면
    // 녹화 중인 채널의 세그먼트를 분리한다. 스냅샷 옵저버는 control 스레드에서
    // 호출되므로(ChannelController가 같은 스레드에서 발행) recCtrl 호출도 control
    // 스레드에서 직렬화돼 안전하다. 직전 상태를 채널별로 기억해 "정상 스트림→끊김"
    // 전이 경계(Reconnecting/Stalled 진입)에서 한 번만 분리한다.
    // recCtrl.onReconnect는 내부에서 녹화 중(splitOnReconnect && Recording)만 동작하므로
    // 비녹화 채널은 무영향. 새 세그먼트는 새 파일 경로로 시작돼 직전 세그먼트를 덮어쓰지 않는다.
    auto prevState = std::make_shared<std::map<std::string, nv::domain::ConnState>>();

    // Fix 3: 옵저버 설정은 control 스레드(executor)에서만 — executor.post로 진입
    // restore post보다 먼저 post되도록 순서 유지 (직렬 보장)
    executor.post([&] {
        mgr.setListChangedObserver(pushList);
        mgr.setSnapshotObserver([&, prevState](const std::string& id,
                                               const nv::app::ChannelSnapshot& s) {
            // 끊김 전이 감지: Streaming/SessionOpen 등 활성 상태에서 Reconnecting/Stalled로
            // 진입하는 순간(드롭 경계)에만 세그먼트를 분리한다. 동일 상태 반복·재시도
            // 대기 중(Reconnecting 유지)·재open(Connecting) 등에서는 중복 분리하지 않는다.
            const auto cur = s.state;
            auto& prev = (*prevState)[id];
            const bool enteringReconnect =
                (cur == nv::domain::ConnState::Reconnecting ||
                 cur == nv::domain::ConnState::Stalled) &&
                prev != nv::domain::ConnState::Reconnecting &&
                prev != nv::domain::ConnState::Stalled;
            if (enteringReconnect) {
                std::string name;
                for (const auto& c : mgr.configs()) {
                    if (c.id == id) { name = c.name; break; }
                }
                recCtrl.onReconnect(id, name);   // 녹화 중인 채널만 내부에서 분리
            }
            prev = cur;
            bridge.publish(QString::fromStdString(id), s);
        });
        // M3-5: RecordingController 옵저버 — 상태 변화 시 UI 스레드로 queued 전달
        recCtrl.setObserver([&](const std::string& id, nv::domain::RecordingState state) {
            const bool rec = (state == nv::domain::RecordingState::Recording);
            if (winPtr != nullptr) {
                QMetaObject::invokeMethod(winPtr, "onRecordingState",
                    Qt::QueuedConnection,
                    Q_ARG(QString, QString::fromStdString(id)),
                    Q_ARG(bool, rec));
            }
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
