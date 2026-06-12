#include "MainWindow.h"
#include <QApplication>
#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include "src/infra/system/ResourceMonitor.h"
#include "src/ui/channels/ChannelDialog.h"
#include "src/ui/channels/ChannelListPanel.h"
#include "src/ui/grid/GridView.h"
#include "src/ui/shell/LogPanel.h"

namespace nv::ui {

static constexpr int kLeftPanelWidth = 200;
static constexpr int kRightPanelWidth = 320;
static constexpr int kPanelToggleWidth = 18;
static constexpr int kHeaderHeight = 32;

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

    // ② 파일 탭 — 자리표시자
    auto* filesTab = new QWidget();
    auto* filesLayout = new QVBoxLayout(filesTab);
    filesLayout->setContentsMargins(12, 12, 12, 12);
    auto* filesPlaceholder = new QLabel(
        QStringLiteral("스냅샷/녹화는 M3에서 구현 예정"), filesTab);
    filesPlaceholder->setAlignment(Qt::AlignCenter);
    filesPlaceholder->setStyleSheet(QStringLiteral("color: #999; font-size: 12px;"));
    filesLayout->addWidget(filesPlaceholder);
    filesLayout->addStretch();
    m_rightTabs->addTab(filesTab, QStringLiteral("파일"));

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
    statusWidget->setFixedHeight(24);
    statusWidget->setStyleSheet(QStringLiteral(
        "background-color: #f3f3f3; border-top: 1px solid #d0d0d0;"));
    auto* statusLayout = new QHBoxLayout(statusWidget);
    statusLayout->setContentsMargins(8, 0, 8, 0);
    statusLayout->setSpacing(16);
    m_statusChannels = new QLabel(QStringLiteral("채널 0 / 전체 0"), statusWidget);
    m_statusChannels->setStyleSheet(QStringLiteral("color: #444; font-size: 11px;"));
    m_statusCpu = new QLabel(QStringLiteral("CPU —"), statusWidget);
    m_statusCpu->setStyleSheet(QStringLiteral("color: #444; font-size: 11px;"));
    m_statusMem = new QLabel(QStringLiteral("메모리 —"), statusWidget);
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

    // ResourceMonitor 1초 주기 갱신
    auto* resMonitor = new nv::infra::ResourceMonitor();
    auto* resTimer = new QTimer(this);
    connect(resTimer, &QTimer::timeout, this, [this, resMonitor] {
        const auto snap = resMonitor->sample();
        if (snap.systemCpuValid)
            m_statusCpu->setText(QStringLiteral("CPU %1%").arg(snap.systemCpuPercent, 0, 'f', 1));
        if (snap.systemMemoryValid && snap.systemMemoryTotalBytes > 0) {
            const double usedGb = snap.systemMemoryUsedBytes / (1024.0 * 1024.0 * 1024.0);
            const double totalGb = snap.systemMemoryTotalBytes / (1024.0 * 1024.0 * 1024.0);
            m_statusMem->setText(QStringLiteral("메모리 %1/%2 GB")
                                     .arg(usedGb, 0, 'f', 1)
                                     .arg(totalGb, 0, 'f', 1));
        }
    });
    resTimer->start(1000);
}

int MainWindow::manualColumns() const {
    return m_colBtnGroup ? m_colBtnGroup->checkedId() : 0;
}

void MainWindow::onChannelList(QVector<QString> ids, QVector<QString> names,
                               QVector<QString> urls, QVector<int> gridIndexes) {
    m_channels.clear();
    for (int i = 0; i < ids.size(); ++i) {
        m_channels.push_back({ids[i].toStdString(), names[i].toStdString(),
                              urls[i].toStdString(), gridIndexes[i]});
    }
    m_channelPanel->updateChannels(m_channels);
    rebuildGrid();
    // 상태바 채널 수 갱신
    m_statusChannels->setText(
        QStringLiteral("전체 %1 채널").arg(static_cast<int>(m_channels.size())));
}

void MainWindow::rebuildGrid() {
    m_grid->rebuild(m_channels, manualColumns());
}

void MainWindow::onSnapshot(QString channelId, QString state, int attempts, QList<int> stages,
                            double pps, qlonglong msSinceLastPacket) {
    m_grid->updateTileStatus(channelId, state, attempts, stages, pps, msSinceLastPacket);
    m_channelPanel->updateStatus(channelId, state);
}

void MainWindow::openAddDialog() {
    ChannelDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted && !dlg.url().isEmpty())
        m_commands.addChannel(dlg.name().toStdString(), dlg.url().toStdString());
}

void MainWindow::openEditDialog(const std::string& id) {
    if (id.empty()) {
        openAddDialog();
        return;
    }
    for (const auto& c : m_channels) {
        if (c.id != id) continue;
        ChannelDialog dlg(this, QString::fromStdString(c.name), QString::fromStdString(c.url));
        if (dlg.exec() == QDialog::Accepted)
            m_commands.updateChannel(id, dlg.name().toStdString(), dlg.url().toStdString());
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

} // namespace nv::ui
