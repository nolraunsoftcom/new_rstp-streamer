#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QMetaObject>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTimer>
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

    // UI 전체가 라이트 톤 고정 스타일시트(#f0f0f0 등)로 구성돼 있어, macOS 다크 모드에서는
    // 스타일을 안 입힌 시스템 다이얼로그(QMessageBox 등)가 다크 팔레트의 흰 글자를 받아
    // "밝은 배경 + 흰 글씨"로 깨진다. 색 구성표를 라이트로 고정해 팔레트를 일치시킨다.
    app.styleHints()->setColorScheme(Qt::ColorScheme::Light);

    // --- infra ---
    nv::infra::SteadyClock clock;
    nv::infra::CompositeLogger logger;
    const QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(cfgDir);
    nv::infra::JsonChannelRepository repo((cfgDir + QStringLiteral("/channels.json")).toStdString());

    // --- control 스레드 + 채널 매니저 ---
    std::atomic<nv::app::ChannelManager*> mgrPtr{nullptr};
    std::atomic<nv::app::RecordingController*> recCtrlPtr{nullptr};
    nv::infra::ControlExecutor executor(1s, [&mgrPtr, &recCtrlPtr] {
        if (auto* m = mgrPtr.load()) m->tickAll();
        if (auto* r = recCtrlPtr.load()) r->tick();
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
    recCtrlPtr.store(&recCtrl);
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
        executor.post([&, id] {
            recCtrl.onChannelRemoved(id);   // 유령 Recording 상태 방지 — 삭제 전 정리
            mgr.removeChannel(id);
        });
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

    // M3-6: 녹화 생존 배선 — 채널 상태 전이 경계에서 RecordingController를 구동한다.
    // 드롭 엣지(정상→Reconnecting/Stalled 진입): onReconnect — 죽어가는 소스에 start하지
    //   않고 현재 세그먼트만 종료(armed 유지).
    // 복구 엣지(Streaming 재도달): onStreaming — armed 채널의 새 세그먼트를 시작.
    // 스냅샷 옵저버는 control 스레드에서 호출되므로(ChannelController가 같은 스레드에서
    // 발행) recCtrl 호출도 control 스레드에서 직렬화돼 안전하다. 직전 상태를 채널별로
    // 기억해 전이 경계에서 한 번씩만 동작한다. 비녹화/비armed 채널은 내부에서 무영향.
    // 새 세그먼트는 새 파일 경로로 시작돼 직전 세그먼트를 덮어쓰지 않는다.
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
            // 복구 엣지: Streaming 재도달(prev != Streaming && cur == Streaming).
            // 끊김 후 복구마다 armed 채널의 새 세그먼트를 시작한다.
            const bool enteringStreaming =
                cur == nv::domain::ConnState::Streaming &&
                prev != nv::domain::ConnState::Streaming;
            if (enteringReconnect || enteringStreaming) {
                std::string name;
                for (const auto& c : mgr.configs()) {
                    if (c.id == id) { name = c.name; break; }
                }
                if (enteringReconnect) {
                    recCtrl.onReconnect(id, name);   // 드롭 엣지: 현재 세그먼트만 종료
                }
                if (enteringStreaming) {
                    recCtrl.onStreaming(id, name);   // 복구 엣지: 새 세그먼트 시작
                }
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

    // --- QA 스트레스 테스트 CLI 훅 (기본 off — 플래그 없으면 동작 없음) ---
    // 파싱: --snapshot-every=N, --record-toggle=N (N은 양의 정수, 초 단위)
    // --auto-record: 각 채널이 Streaming 재도달 시 자동 녹화 시작
    // --record-toggle=N 과 --auto-record 동시 사용 금지 — record-toggle 우선
    const QStringList cliArgs = QApplication::arguments();

    const bool qaAutoRecord = cliArgs.contains(QStringLiteral("--auto-record"));

    auto parseIntFlag = [&](const QString& prefix) -> int {
        for (const QString& arg : cliArgs) {
            if (arg.startsWith(prefix)) {
                bool ok = false;
                const int v = arg.mid(prefix.size()).toInt(&ok);
                if (ok && v > 0) return v;
                fprintf(stderr, "[QA] 경고: '%s' 값이 잘못됐습니다(무시됨)\n",
                        arg.toUtf8().constData());
            }
        }
        return 0;
    };
    const int qaSnapshotEvery = parseIntFlag(QStringLiteral("--snapshot-every="));
    const int qaRecordToggle  = parseIntFlag(QStringLiteral("--record-toggle="));

    // --auto-record: onStreaming 복구 엣지에서 armed 상태 없이 자동 녹화 시작
    // record-toggle이 있으면 auto-record는 무시
    if (qaAutoRecord && qaRecordToggle == 0) {
        fprintf(stderr, "[QA] auto-record on\n");
        // 기존 snapshotObserver(onStreaming 경로)에 QA 훅을 얹는다.
        // executor 내부에서만 호출되므로 thread-safe.
        executor.post([&] {
            mgr.setSnapshotObserver([&, prevState](const std::string& id,
                                                    const nv::app::ChannelSnapshot& s) {
                const auto cur = s.state;
                auto& prev = (*prevState)[id];
                const bool enteringReconnect =
                    (cur == nv::domain::ConnState::Reconnecting ||
                     cur == nv::domain::ConnState::Stalled) &&
                    prev != nv::domain::ConnState::Reconnecting &&
                    prev != nv::domain::ConnState::Stalled;
                const bool enteringStreaming =
                    cur == nv::domain::ConnState::Streaming &&
                    prev != nv::domain::ConnState::Streaming;
                if (enteringReconnect || enteringStreaming) {
                    std::string name;
                    for (const auto& c : mgr.configs()) {
                        if (c.id == id) { name = c.name; break; }
                    }
                    if (enteringReconnect) {
                        recCtrl.onReconnect(id, name);
                    }
                    if (enteringStreaming) {
                        recCtrl.onStreaming(id, name);
                        // QA: armed가 아닌 채널도 Streaming 진입 시 자동 녹화 시작
                        if (recCtrl.stateOf(id) != nv::domain::RecordingState::Recording) {
                            recCtrl.toggle(id, name);
                            fprintf(stderr, "[QA] auto-record started ch=%s\n", id.c_str());
                        }
                    }
                }
                prev = cur;
                bridge.publish(QString::fromStdString(id), s);
            });
        });
    }

    // --snapshot-every=N: N초마다 전 채널 스냅샷 (UI 스레드 타이머 → executor.post)
    if (qaSnapshotEvery > 0) {
        auto* snapTimer = new QTimer(&app);
        snapTimer->setInterval(qaSnapshotEvery * 1000);
        QObject::connect(snapTimer, &QTimer::timeout, [&] {
            executor.post([&] {
                fprintf(stderr, "[QA] snapshot tick\n");
                for (const auto& c : mgr.configs()) {
                    const std::string path = nv::infra::RecordingPaths::snapshotPath(c.name);
                    snapSvc.capture(c.id, c.name, path);
                }
            });
        });
        snapTimer->start();
    }

    // --record-toggle=N: N초마다 전 채널 녹화 토글 (UI 스레드 타이머 → executor.post)
    if (qaRecordToggle > 0) {
        auto* toggleTimer = new QTimer(&app);
        toggleTimer->setInterval(qaRecordToggle * 1000);
        QObject::connect(toggleTimer, &QTimer::timeout, [&] {
            executor.post([&] {
                for (const auto& c : mgr.configs()) {
                    fprintf(stderr, "[QA] record toggle ch=%s\n", c.id.c_str());
                    recCtrl.toggle(c.id, c.name);
                }
            });
        });
        toggleTimer->start();
    }
    // --- QA 훅 끝 ---

    const bool autoConnect = QApplication::arguments().contains(QStringLiteral("--connect"));
    executor.post([&, autoConnect] { mgr.restore(autoConnect); });

    // --- 소크 통계 (60초마다 RSS + 표시 fps 기록 — 4컬럼) ---
    const QString logsDir = cfgDir + QStringLiteral("/logs");
    QDir().mkpath(logsDir);
    nv::infra::SoakLogger soakLogger(factory, logsDir + QStringLiteral("/soak.csv"));
    soakLogger.start(60'000);

    const int rc = QApplication::exec();
    // Fix 4: 명시적 teardown — 콜백 해제 → 채널 정리 → 큐 비움 (스택 수명 의존 제거)
    recCtrlPtr.store(nullptr);    // tick 람다가 dangling recCtrl을 보지 않도록 drain 전에 해제
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
