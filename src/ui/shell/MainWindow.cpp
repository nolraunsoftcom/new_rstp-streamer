#include "MainWindow.h"
#include <QApplication>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QUrl>
#include <QSet>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include "src/ui/channels/ChannelDialog.h"
#include "src/ui/channels/ChannelInfoDialog.h"
#include "src/ui/channels/ChannelListPanel.h"
#include "src/ui/grid/GridView.h"
#include "src/ui/shell/LogPanel.h"
#include "src/ui/panels/FilePanel.h"
#include "src/ui/shell/Toast.h"

namespace nv::ui {

static constexpr int kLeftPanelWidth = 200;
static constexpr int kRightPanelWidth = 280;
static constexpr int kPanelToggleWidth = 18;
static constexpr int kHeaderHeight = 32;

namespace {
// 레거시(../viewer) formatToastDuration/formatToastBytes 미러 — 토스트 문구 동일성.
QString formatToastDuration(int seconds)
{
    if (seconds < 0) seconds = 0;
    const int h = seconds / 3600;
    const int m = (seconds / 60) % 60;
    const int s = seconds % 60;
    if (h > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

QString formatToastBytes(qint64 bytes)
{
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    const double kib = bytes / 1024.0;
    if (kib < 1024.0) return QStringLiteral("%1 KB").arg(kib, 0, 'f', 1);
    const double mib = kib / 1024.0;
    if (mib < 1024.0) return QStringLiteral("%1 MB").arg(mib, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(mib / 1024.0, 0, 'f', 2);
}

// 전체화면 탭 라벨 HTML — 항상 ● 자리를 확보(폭 고정). 대기=투명, 녹화=빨강, 시작=노랑.
// 색만 바뀌고 글자 수가 동일해 상태 전이 시 라벨 폭이 변하지 않아 이름이 잘리지 않는다.
QString recTabHtml(const QString& base, nv::domain::RecordingState st)
{
    QString color = QStringLiteral("rgba(0,0,0,0)");   // 대기: 투명(자리만 차지)
    if (st == nv::domain::RecordingState::Recording)    color = QStringLiteral("#ff4040");
    else if (st == nv::domain::RecordingState::Starting) color = QStringLiteral("#e8a838");
    return QStringLiteral("<span style='color:%1'>●</span> %2").arg(color, base.toHtmlEscaped());
}
} // namespace

static const QString kTabStyle = QStringLiteral(
    "QTabWidget::pane { border: none; background-color: #f5f5f5; }"
    "QTabWidget::tab-bar { left: 0px; }"
    "QTabBar { background-color: #f3f3f3; border-bottom: 1px solid #d0d0d0; }"
    "QTabBar::tab { background-color: #f3f3f3; color: #555; border: none; "
    "border-bottom: 1px solid #d0d0d0; "
    "padding: 0px; font-size: 11px; min-width: 86px; min-height: 30px; }"
    "QTabBar::tab:selected { background-color: #ffffff; color: #111; "
    "border-bottom: 2px solid #0078d4; }");

static const QString kVideoTabStyle = QStringLiteral(
    "QTabWidget::pane { border: none; background-color: black; }"
    "QTabWidget::tab-bar { left: 0px; }"
    "QTabBar { background-color: #f3f3f3; border-bottom: 1px solid #d0d0d0; }"
    "QTabBar::tab { background-color: #f3f3f3; color: #555; border: none; "
    "border-bottom: 1px solid #d0d0d0; "
    "padding: 0px; font-size: 11px; min-width: 120px; min-height: 30px; }"
    "QTabBar::tab:selected { background-color: #ffffff; color: #111; "
    "border-bottom: 2px solid #0078d4; }");

static QPushButton* makeToggleButton(QWidget* parent) {
    auto* btn = new QPushButton(parent);
    btn->setFixedWidth(kPanelToggleWidth);
    btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(QStringLiteral(
        "QPushButton { color: #555; background-color: #f3f3f3; border: none; "
        "border-left: 1px solid #d0d0d0; border-right: 1px solid #d0d0d0; "
        "font-size: 12px; padding: 0; }"
        "QPushButton:hover { color: #111; background-color: #e8f2ff; }"
        "QPushButton:pressed { background-color: #cfe8ff; }"));
    return btn;
}

MainWindow::MainWindow(GridView* grid, ChannelListPanel* channelPanel, LogPanel* logPanel,
                       Commands commands)
    : m_commands(std::move(commands))
    , m_grid(grid)
    , m_channelPanel(channelPanel)
    , m_logPanel(logPanel) {
    setWindowTitle(QStringLiteral("영상관리시스템"));
    resize(1400, 800);

    // ── 중앙 위젯 전체 ──────────────────────────────────────
    auto* central = new QWidget(this);
    central->setStyleSheet(QStringLiteral("background-color: #f0f0f0;"));
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    // ── 패널 행 ─────────────────────────────────────────────
    auto* panelRow = new QWidget(central);
    auto* mainLayout = new QHBoxLayout(panelRow);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ── 좌측 패널 (ChannelListPanel) ─────────────────────────
    m_leftPanel = channelPanel;
    channelPanel->setParent(panelRow);
    channelPanel->setFixedWidth(kLeftPanelWidth);
    channelPanel->setStyleSheet(QStringLiteral("background-color: #f5f5f5;"));
    mainLayout->addWidget(channelPanel);

    // 좌측 토글 버튼
    auto* leftToggle = makeToggleButton(panelRow);
    m_leftToggle = leftToggle;
    connect(leftToggle, &QPushButton::clicked, this,
            [this] { setLeftPanelVisible(!m_leftVisible); });
    mainLayout->addWidget(leftToggle);

    // ── 중앙 영상 영역 ───────────────────────────────────────
    auto* videoArea = new QWidget(panelRow);
    videoArea->setObjectName(QStringLiteral("videoArea"));
    videoArea->setStyleSheet(QStringLiteral(
        "#videoArea { background-color: #000000; "
        "border-left: 1px solid #c8c8c8; border-right: 1px solid #c8c8c8; }"));
    auto* videoLayout = new QVBoxLayout(videoArea);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    videoLayout->setSpacing(0);

    auto* videoTabs = new QTabWidget(videoArea);
    m_videoTabs = videoTabs;
    videoTabs->setTabsClosable(false);
    videoTabs->tabBar()->setExpanding(false);
    videoTabs->tabBar()->setDocumentMode(true);
    videoTabs->tabBar()->setFixedHeight(kHeaderHeight);
    videoTabs->setStyleSheet(kVideoTabStyle);

    // "전체" 탭 — GridView가 자체 QScrollArea를 포함 (레거시 구조)
    auto* gridPage = new QWidget();
    auto* gridPageLayout = new QVBoxLayout(gridPage);
    gridPageLayout->setContentsMargins(0, 0, 0, 0);
    gridPageLayout->setSpacing(0);

    grid->setParent(gridPage);
    gridPageLayout->addWidget(grid, 1);

    videoTabs->addTab(gridPage, QStringLiteral("전체"));
    videoTabs->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    videoTabs->tabBar()->setTabButton(0, QTabBar::LeftSide, nullptr);

    videoLayout->addWidget(videoTabs, 1);
    mainLayout->addWidget(videoArea, 1);

    // 우측 토글 버튼
    auto* rightToggle = makeToggleButton(panelRow);
    m_rightToggle = rightToggle;
    connect(rightToggle, &QPushButton::clicked, this,
            [this] { setRightPanelVisible(!m_rightVisible); });
    mainLayout->addWidget(rightToggle);

    // ── 우측 패널 (설정/파일/로그 탭) ───────────────────────
    auto* rightPanel = new QWidget(panelRow);
    m_rightPanel = rightPanel;
    rightPanel->setFixedWidth(kRightPanelWidth);
    rightPanel->setStyleSheet(QStringLiteral("background-color: #f5f5f5;"));
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    m_rightTabs = new QTabWidget(rightPanel);
    m_rightTabs->tabBar()->setExpanding(false);
    m_rightTabs->tabBar()->setDocumentMode(true);
    m_rightTabs->tabBar()->setFixedHeight(kHeaderHeight);
    m_rightTabs->setStyleSheet(kTabStyle);

    // ① 설정 탭 — 그리드 컬럼 버튼 그룹
    auto* settingsTab = new QWidget();
    auto* settingsLayout = new QVBoxLayout(settingsTab);
    settingsLayout->setContentsMargins(12, 12, 12, 12);
    settingsLayout->setSpacing(16);

    auto* colLabel = new QLabel(QStringLiteral("그리드 컬럼"), settingsTab);
    colLabel->setStyleSheet(QStringLiteral("color: #555; font-size: 10px;"));
    settingsLayout->addWidget(colLabel);

    auto* colBtnLayout = new QHBoxLayout();
    colBtnLayout->setSpacing(4);
    m_colBtnGroup = new QButtonGroup(this);
    m_colBtnGroup->setExclusive(true);
    const QStringList colLabels = {QStringLiteral("Auto"), QStringLiteral("1"),
                                   QStringLiteral("2"),    QStringLiteral("3"),
                                   QStringLiteral("4"),    QStringLiteral("5")};
    const QList<int> colValues = {0, 1, 2, 3, 4, 5};
    for (int i = 0; i < colLabels.size(); ++i) {
        auto* btn = new QPushButton(colLabels[i], settingsTab);
        btn->setCheckable(true);
        btn->setFixedHeight(28);
        btn->setMinimumWidth(38);
        btn->setStyleSheet(QStringLiteral(
            "QPushButton { color: #333; background-color: #ffffff; border: 1px solid #bdbdbd; "
            "border-radius: 3px; font-size: 11px; padding: 0 8px; }"
            "QPushButton:checked { color: #111; background-color: #cfe8ff; border-color: #0078d4; }"
            "QPushButton:hover { background-color: #e8f2ff; }"));
        m_colBtnGroup->addButton(btn, colValues[i]);
        colBtnLayout->addWidget(btn);
        if (colValues[i] == 0) btn->setChecked(true);
    }
    connect(m_colBtnGroup, &QButtonGroup::idClicked, this, [this](int) { rebuildGrid(); });
    settingsLayout->addLayout(colBtnLayout);
    settingsLayout->addStretch();
    m_rightTabs->addTab(settingsTab, QStringLiteral("설정"));

    // ② 파일 탭 — M3-5: FilePanel (녹화/스냅샷 목록)
    m_filePanel = new FilePanel(rightPanel);
    m_rightTabs->addTab(m_filePanel, QStringLiteral("스냅샷/녹화"));

    // ③ 로그 탭
    logPanel->setParent(rightPanel);
    m_rightTabs->addTab(logPanel, QStringLiteral("로그"));

    rightLayout->addWidget(m_rightTabs);
    mainLayout->addWidget(rightPanel);

    centralLayout->addWidget(panelRow, 1);

    // ── 상태바 구분선 ────────────────────────────────────────
    auto* separator = new QFrame(central);
    separator->setFixedHeight(1);
    separator->setStyleSheet(QStringLiteral("background-color: #d0d0d0; border: none;"));
    centralLayout->addWidget(separator);

    // ── 상태바 ───────────────────────────────────────────────
    auto* statusWidget = new QWidget(central);
    statusWidget->setFixedHeight(30);
    statusWidget->setObjectName(QStringLiteral("statusBar"));
    statusWidget->setStyleSheet(QStringLiteral(
        "#statusBar { background-color: #f3f3f3; border: none; }"));
    auto* statusLayout = new QHBoxLayout(statusWidget);
    statusLayout->setContentsMargins(8, 0, 8, 0);
    statusLayout->setSpacing(16);
    m_statusChannels = new QLabel(
        QStringLiteral("Connected: 0/0 | Bitrate: 0.0 Mbps | FPS: 0.0 | Dropped: 0"), statusWidget);
    m_statusChannels->setStyleSheet(QStringLiteral("color: #222; font-size: 11px;"));
    m_statusCpu = new QLabel(QStringLiteral("PC CPU: --"), statusWidget);
    m_statusCpu->setStyleSheet(QStringLiteral("color: #444; font-size: 11px;"));
    m_statusMem = new QLabel(QStringLiteral("PC RAM: --"), statusWidget);
    m_statusMem->setStyleSheet(QStringLiteral("color: #444; font-size: 11px;"));
    statusLayout->addWidget(m_statusChannels);
    statusLayout->addStretch();
    statusLayout->addWidget(m_statusCpu);
    statusLayout->addWidget(m_statusMem);
    centralLayout->addWidget(statusWidget);

    setCentralWidget(central);
    setStatusBar(nullptr);  // 기본 QStatusBar 숨김 — 커스텀 상태바 사용

    // 패널 토글 버튼 초기 텍스트
    updateToggleButtons();

    // ResourceMonitor 1초 주기 갱신 (Fix 5: unique_ptr 멤버 소유)
    m_resourceMonitor = std::make_unique<nv::infra::ResourceMonitor>();
    auto* resTimer = new QTimer(this);
    connect(resTimer, &QTimer::timeout, this, [this] {
        const auto snap = m_resourceMonitor->sample();
        if (snap.systemCpuValid)
            m_statusCpu->setText(QStringLiteral("PC CPU: %1%").arg(snap.systemCpuPercent, 0, 'f', 1));
        if (snap.systemMemoryValid && snap.systemMemoryTotalBytes > 0) {
            const double usedGb = snap.systemMemoryUsedBytes / (1024.0 * 1024.0 * 1024.0);
            const double totalGb = snap.systemMemoryTotalBytes / (1024.0 * 1024.0 * 1024.0);
            m_statusMem->setText(QStringLiteral("PC RAM: %1 / %2 GB")
                                     .arg(usedGb, 0, 'f', 2)
                                     .arg(totalGb, 0, 'f', 2));
        }
    });
    resTimer->start(1000);
}

int MainWindow::manualColumns() const {
    return m_colBtnGroup ? m_colBtnGroup->checkedId() : 0;
}

void MainWindow::onChannelList(QVector<QString> ids, QVector<QString> names,
                               QVector<QString> urls, QVector<int> gridIndexes,
                               QVector<int> listIndexes, QVector<bool> autoConnects,
                               QVector<bool> useRelays) {
    m_channels.clear();
    // 제거된 채널을 m_streaming에서 정리: 현재 id 집합에 없으면 삭제
    QSet<QString> currentIds(ids.begin(), ids.end());
    for (auto it = m_streaming.begin(); it != m_streaming.end(); ) {
        if (!currentIds.contains(it.key()))
            it = m_streaming.erase(it);
        else
            ++it;
    }
    // 삭제된 채널의 전체화면 탭은 닫는다(뒤에서 앞으로 — removeTab 인덱스 시프트 방지).
    if (m_videoTabs != nullptr) {
        for (int i = m_videoTabs->count() - 1; i >= 1; --i) {
            const QString tabId = m_videoTabs->widget(i)->property("nvChannelId").toString();
            if (!currentIds.contains(tabId)) closeFullscreenTab(i);
        }
    }
    for (int i = 0; i < ids.size(); ++i) {
        nv::domain::ChannelConfig cfg;
        cfg.id          = ids[i].toStdString();
        cfg.name        = names[i].toStdString();
        cfg.url         = urls[i].toStdString();
        cfg.gridIndex   = gridIndexes[i];
        cfg.listIndex   = i < listIndexes.size() ? listIndexes[i] : gridIndexes[i];
        cfg.autoConnect = i < autoConnects.size() ? autoConnects[i] : false;
        cfg.useRelay    = i < useRelays.size() ? useRelays[i] : false;
        m_channels.push_back(std::move(cfg));
        // 신규 채널은 미연결(false) 초기값 설정
        if (!m_streaming.contains(ids[i]))
            m_streaming[ids[i]] = false;
    }
    // 그리드: gridIndex 순서(transport가 이미 gridIndex 정렬). 리스트: listIndex 순서(독립).
    std::vector<nv::domain::ChannelConfig> listOrdered = m_channels;
    std::sort(listOrdered.begin(), listOrdered.end(),
              [](const auto& a, const auto& b) { return a.listIndex < b.listIndex; });
    m_channelPanel->updateChannels(listOrdered);
    rebuildGrid();
    updateStatusBar();
}

void MainWindow::rebuildGrid() {
    m_grid->rebuild(m_channels, manualColumns());
}

void MainWindow::onSnapshot(QString channelId, QString state, int attempts, QList<int> stages,
                            double pps, qlonglong msSinceLastPacket, QString reason,
                            double bitrateKbps, qlonglong droppedFrames,
                            qlonglong decodedFrames, qlonglong displayedFrames,
                            qlonglong readBytesTotal) {
    m_grid->updateTileStatus(channelId, state, attempts, stages, pps, msSinceLastPacket, reason);
    m_channelPanel->updateStatus(channelId, state, reason);
    // A1: Streaming 여부 맵 갱신
    m_streaming[channelId] = (state == QStringLiteral("Streaming"));
    // 상태바/채널정보 집계용 채널별 지표 캐시
    m_pps[channelId]         = pps;
    m_bitrateKbps[channelId] = bitrateKbps;
    m_dropped[channelId]     = droppedFrames;
    m_decoded[channelId]     = decodedFrames;
    m_displayed[channelId]   = displayedFrames;
    m_readBytes[channelId]   = readBytesTotal;
    updateStatusBar();
}

void MainWindow::updateStatusBar() {
    // 레거시 StatusBar::updateStats 미러: Connected | Bitrate(평균 Mbps) | FPS(평균) | Dropped(합)
    const int total = static_cast<int>(m_channels.size());
    int connected = 0;
    double sumBitrateKbps = 0.0;
    double sumFps = 0.0;
    qlonglong sumDropped = 0;
    for (const auto& c : m_channels) {
        const QString id = QString::fromStdString(c.id);
        if (!m_streaming.value(id, false)) continue;
        ++connected;
        sumBitrateKbps += m_bitrateKbps.value(id, 0.0);
        sumFps         += m_pps.value(id, 0.0);
        sumDropped     += m_dropped.value(id, 0);
    }
    const double avgMbps = connected > 0 ? sumBitrateKbps / connected / 1000.0 : 0.0;
    const double avgFps  = connected > 0 ? sumFps / connected : 0.0;

    const QString text = QStringLiteral("Connected: %1/%2 | Bitrate: %3 Mbps | FPS: %4 | Dropped: %5")
        .arg(connected).arg(total)
        .arg(avgMbps, 0, 'f', 1)
        .arg(avgFps, 0, 'f', 1)
        .arg(sumDropped);

    // 전 채널 끊김 경보: 채널이 있는데 연결 0이면 빨강 강조
    const bool alarm = (total > 0 && connected == 0);
    m_statusChannels->setText(text);
    m_statusChannels->setStyleSheet(alarm
        ? QStringLiteral("color: #d13438; font-weight: bold; font-size: 11px;")
        : QStringLiteral("color: #222; font-size: 11px;"));
}

void MainWindow::openAddDialog() {
    ChannelDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted && !dlg.url().isEmpty())
        m_commands.addChannel(dlg.name().toStdString(), dlg.url().toStdString(),
                              dlg.autoConnect(), dlg.useRelay());
}

void MainWindow::openEditDialog(const std::string& id) {
    if (id.empty()) {
        openAddDialog();
        return;
    }
    for (const auto& c : m_channels) {
        if (c.id != id) continue;
        ChannelDialog dlg(this, QString::fromStdString(c.name), QString::fromStdString(c.url),
                          c.autoConnect, c.useRelay);
        if (dlg.exec() == QDialog::Accepted)
            m_commands.updateChannel(id, dlg.name().toStdString(), dlg.url().toStdString(),
                                     dlg.autoConnect(), dlg.useRelay());
        return;
    }
}

void MainWindow::setLeftPanelVisible(bool visible) {
    if (m_leftVisible == visible) return;
    m_leftVisible = visible;
    if (m_leftPanel) m_leftPanel->setVisible(visible);
    updateToggleButtons();
}

void MainWindow::setRightPanelVisible(bool visible) {
    if (m_rightVisible == visible) return;
    m_rightVisible = visible;
    if (m_rightPanel) m_rightPanel->setVisible(visible);
    updateToggleButtons();
}

void MainWindow::updateToggleButtons() {
    if (auto* btn = qobject_cast<QPushButton*>(m_leftToggle)) {
        btn->setText(m_leftVisible ? QStringLiteral("◀") : QStringLiteral("▶"));
        btn->setToolTip(m_leftVisible ? QStringLiteral("왼쪽 패널 숨기기")
                                      : QStringLiteral("왼쪽 패널 보이기"));
    }
    if (auto* btn = qobject_cast<QPushButton*>(m_rightToggle)) {
        btn->setText(m_rightVisible ? QStringLiteral("▶") : QStringLiteral("◀"));
        btn->setToolTip(m_rightVisible ? QStringLiteral("오른쪽 패널 숨기기")
                                       : QStringLiteral("오른쪽 패널 보이기"));
    }
}

void MainWindow::onRecordingState(QString channelId, nv::domain::RecordingState state)
{
    m_recStates[channelId] = state;   // 전체화면 탭 prime용 캐시
    // 타일 정보바 ● 버튼 색 + REC 뱃지 갱신
    m_grid->updateRecordingState(channelId, state);
    // 전체화면 탭이 열려있으면 탭 라벨에 녹화 ● 표시 갱신
    updateFullscreenTabRecording(channelId, state);
    // 녹화 중지 시 파일 패널 갱신 (새 MKV 파일이 생겼을 수 있음)
    if (state == nv::domain::RecordingState::Idle && m_filePanel) {
        m_filePanel->refresh();
    }
}

void MainWindow::updateFullscreenTabRecording(const QString& channelId,
                                              nv::domain::RecordingState state)
{
    if (m_videoTabs == nullptr) return;
    for (int i = 1; i < m_videoTabs->count(); ++i) {
        if (m_videoTabs->widget(i)->property("nvChannelId").toString() != channelId) continue;
        auto* lbl = qobject_cast<QLabel*>(
            m_videoTabs->tabBar()->tabButton(i, QTabBar::LeftSide));
        if (lbl == nullptr) return;
        lbl->setText(recTabHtml(lbl->property("nvBaseName").toString(), state));
        return;
    }
}


// ── P3: 토스트 알림 슬롯 ──────────────────────────────────────────────────────
// control→UI queued 호출; 기존 이벤트 흐름만 사용, 백엔드 불변.

void MainWindow::onSnapshotSaved(QString /*channelName*/, QString filePath)
{
    // 레거시: "스냅샷 저장됨" / detail = 파일명 / 액션 [폴더][열기]
    const QString fileName = filePath.section(QLatin1Char('/'), -1);
    const QString path = filePath;
    Toast::show(centralWidget(),
                QStringLiteral("스냅샷 저장됨"),
                fileName,
                Toast::Level::Info,
                3500,
                {{QStringLiteral("폴더"), [path]() {
                      QDesktopServices::openUrl(
                          QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
                  }},
                 {QStringLiteral("열기"), [path]() {
                      QDesktopServices::openUrl(QUrl::fromLocalFile(path));
                  }}});
}

void MainWindow::onRecordingSaved(QString channelName, QString filePath,
                                  bool autoSaved, qint64 bytes, int durationSec)
{
    // 레거시: "녹화 자동 저장됨" or "녹화 저장됨"
    // detail: "채널명 · 0:00 · 0.0 KB" (formatToastDuration/Bytes 미러) / 액션 [폴더][재생]
    // bytes: stopRecording이 비동기라 옵저버 시점엔 파일 미마감 → UI 슬롯(큐 이후)에서
    //        실제 파일 크기를 stat한다. 경로가 비면 전달된 bytes를 사용(폴백).
    const qint64 actualBytes = filePath.isEmpty() ? bytes : QFileInfo(filePath).size();
    const QString detail = QStringLiteral("%1 · %2 · %3")
        .arg(channelName, formatToastDuration(durationSec), formatToastBytes(actualBytes));
    const QString path = filePath;
    Toast::show(centralWidget(),
                autoSaved ? QStringLiteral("녹화 자동 저장됨")
                          : QStringLiteral("녹화 저장됨"),
                detail,
                autoSaved ? Toast::Level::Warn : Toast::Level::Info,
                5000,
                {{QStringLiteral("폴더"), [path]() {
                      QDesktopServices::openUrl(
                          QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
                  }},
                 {QStringLiteral("재생"), [path]() {
                      QDesktopServices::openUrl(QUrl::fromLocalFile(path));
                  }}});
}

void MainWindow::onRecordingFailed(QString channelName, QString reason)
{
    // 레거시: "녹화 실패" / detail = "채널명 · 사유" / 액션 [로그 보기]
    const QString detail = QStringLiteral("%1 · %2").arg(channelName, reason);
    Toast::show(centralWidget(),
                QStringLiteral("녹화 실패"),
                detail,
                Toast::Level::Error,
                9000,
                {{QStringLiteral("로그 보기"), [this]() {
                      setRightPanelVisible(true);
                      if (m_rightTabs) m_rightTabs->setCurrentIndex(2);  // ③ 로그 탭
                  }}});
}

// 레거시 openFullscreenTab 미러 — 단, 우리 구조는 프레임을 IFrameSurfaceRegistry에서
// channelId로 공유하므로 전체화면 뷰는 2차 스트림 없이 같은 프레임을 큰 위젯으로 렌더한다.
void MainWindow::openFullscreenTab(const std::string& id)
{
    if (m_videoTabs == nullptr) return;
    const QString qid = QString::fromStdString(id);

    // 이미 열려있으면 해당 탭으로 전환
    for (int i = 1; i < m_videoTabs->count(); ++i) {
        if (m_videoTabs->widget(i)->property("nvChannelId").toString() == qid) {
            m_videoTabs->setCurrentIndex(i);
            return;
        }
    }
    if (!m_commands.makeFullscreenView) return;
    QWidget* view = m_commands.makeFullscreenView(id);
    if (view == nullptr) return;
    view->setProperty("nvChannelId", qid);

    // 채널명 조회(UI 캐시)
    QString name = qid;
    for (const auto& c : m_channels)
        if (c.id == id) { name = QString::fromStdString(c.name); break; }

    const int idx = m_videoTabs->addTab(view, QString());

    // 좌측 라벨(채널명) + 우측 닫기 × (레거시 탭 버튼 구성)
    // 초기 텍스트에 ● 자리(투명)를 포함해 라벨 폭을 "● 이름" 기준으로 확정 → 녹화 시 안 잘림.
    auto* tabLabel = new QLabel(recTabHtml(name, nv::domain::RecordingState::Idle));
    tabLabel->setProperty("nvBaseName", name);   // 녹화 ● 표시 갱신 시 원래 이름 복원용
    tabLabel->setStyleSheet(QStringLiteral(
        "color: #333; font-size: 11px; padding-left: 8px; background-color: transparent;"));
    m_videoTabs->tabBar()->setTabButton(idx, QTabBar::LeftSide, tabLabel);

    auto* closeBtn = new QPushButton(QStringLiteral("×"));
    closeBtn->setFixedSize(18, 18);
    closeBtn->setStyleSheet(QStringLiteral(
        "QPushButton { color: #666; background-color: transparent; border: none; "
        "font-size: 12px; padding: 0; margin: 0; }"
        "QPushButton:hover { color: #111; background-color: #dbeafe; border-radius: 3px; }"));
    connect(closeBtn, &QPushButton::clicked, this, [this, view]() {
        for (int i = 1; i < m_videoTabs->count(); ++i)
            if (m_videoTabs->widget(i) == view) { closeFullscreenTab(i); return; }
    });
    m_videoTabs->tabBar()->setTabButton(idx, QTabBar::RightSide, closeBtn);

    m_videoTabs->setCurrentIndex(idx);

    // 이미 녹화 중인 채널이면 탭 열자마자 녹화 ● 표시 반영
    updateFullscreenTabRecording(qid,
        m_recStates.value(qid, nv::domain::RecordingState::Idle));
}

void MainWindow::openChannelInfo(const std::string& id)
{
    const nv::domain::ChannelConfig* cfg = nullptr;
    for (const auto& c : m_channels)
        if (c.id == id) { cfg = &c; break; }
    if (cfg == nullptr) return;

    const QString qid = QString::fromStdString(id);
    // 라이브 통계 provider — 다이얼로그가 1초마다 호출. this/qid 캡처는 다이얼로그가
    // this의 자식(WA_DeleteOnClose)이라 수명 안전.
    auto provider = [this, qid]() -> ChannelInfoDialog::Stats {
        ChannelInfoDialog::Stats s;
        s.streaming        = m_streaming.value(qid, false);
        s.outputFps        = m_pps.value(qid, 0.0);
        s.bitrateKbps      = m_bitrateKbps.value(qid, 0.0);
        s.droppedFrames    = m_dropped.value(qid, 0);
        s.decodedFrames    = m_decoded.value(qid, 0);
        s.displayedFrames  = m_displayed.value(qid, 0);
        s.readBytesTotal   = m_readBytes.value(qid, 0);
        return s;
    };

    auto* dlg = new ChannelInfoDialog(*cfg, std::move(provider), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MainWindow::closeFullscreenTab(int index)
{
    if (m_videoTabs == nullptr || index <= 0) return;   // 0 = "전체" 그리드 탭 보호
    QWidget* w = m_videoTabs->widget(index);
    m_videoTabs->removeTab(index);
    if (w != nullptr) w->deleteLater();   // 공유 레지스트리라 스트림 teardown 불필요
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    // 창 크기 변경 시 토스트 위치 재조정 (레거시 positionToast 미러)
    Toast::reposition(centralWidget());
}

} // namespace nv::ui
