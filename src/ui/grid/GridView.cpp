#include "GridView.h"
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>
#include "src/domain/layout/GridRules.h"
#include "src/infra/ffmpeg/ChannelSourceFactory.h"
#include "src/infra/video/LatestFrameSlot.h"
#include "src/ui/grid/VideoTileWidget.h"

namespace nv::ui {

// ── 레거시 상수 (MainWindow.cpp 기준) ─────────────────────────────────────
static constexpr int kInfoBarHeight = 28;   // VIEWER_INFO_BAR_HEIGHT
static constexpr int kGridSpacing   = 1;    // GRID_SPACING

// ── 레거시 Style.h TOOL_BUTTON ────────────────────────────────────────────
static const QString kToolButton = QStringLiteral(
    "QPushButton { color: #3a3a3a; background: transparent; border: none; "
    "padding: 0; margin: 0 2px; font-size: 12px; min-width: 24px; min-height: 20px; }"
    "QPushButton:hover { color: #0f62fe; }"
    "QPushButton:disabled { color: #b0b0b0; }");

// ── 상태 → 표시 문구 (레거시 VlcWidget::statusText()) + 색 ──────────────
static QString statusTextFor(const QString& state) {
    if (state == QStringLiteral("Streaming"))   return QStringLiteral("연결됨");
    if (state == QStringLiteral("Connecting"))  return QStringLiteral("연결 중");
    if (state == QStringLiteral("SessionOpen")) return QStringLiteral("연결 중");
    if (state == QStringLiteral("Reconnecting"))return QStringLiteral("재접속 중");
    if (state == QStringLiteral("Stalled"))     return QStringLiteral("재접속 중");
    if (state == QStringLiteral("Failed"))      return QStringLiteral("실패");
    if (state == QStringLiteral("Idle"))        return QStringLiteral("대기");
    return state;
}

// 패킷 경과 시간 기반 색 (레거시 팔레트)
static const char* packetColor(qlonglong msSince) {
    if (msSince < 0)    return "#666";      // 이력없음
    if (msSince < 1000) return "#12823b";   // 정상
    if (msSince < 3000) return "#e8a838";   // 1~3s
    return "#d13438";                        // 3s+
}

// ── Tile: 정보바 + VideoTileWidget ───────────────────────────────────────
struct GridView::Tile : public QWidget {
    QLabel*          nameLabel   = nullptr;
    QLabel*          packetLabel = nullptr;
    QPushButton*     snapBtn     = nullptr;
    QPushButton*     recBtn      = nullptr;
    VideoTileWidget* video       = nullptr;
    std::string      channelId;
    QString          name;

    Tile(nv::infra::LatestFrameSlot& slot, std::string id, QString nm, QWidget* parent)
        : QWidget(parent), channelId(std::move(id)), name(std::move(nm))
    {
        auto* mainLay = new QVBoxLayout(this);
        mainLay->setContentsMargins(0, 0, 0, 0);
        mainLay->setSpacing(0);

        // ── 상단 정보바 (레거시 VlcWidget.cpp 61~119행) ──────────────────
        auto* infoBar = new QWidget(this);
        infoBar->setStyleSheet(
            QStringLiteral("background-color:#f3f3f3; border-bottom:1px solid #d0d0d0;"));
        infoBar->setFixedHeight(kInfoBarHeight);

        nameLabel = new QLabel(name, infoBar);
        nameLabel->setStyleSheet(
            QStringLiteral("color:#222; font-size:11px; font-weight:bold;"));

        packetLabel = new QLabel(QStringLiteral("패킷 —"), infoBar);
        packetLabel->setStyleSheet(
            QStringLiteral("color:#666; font-size:10px; background:transparent;"));

        snapBtn = new QPushButton(QStringLiteral("📷"), infoBar);
        snapBtn->setFixedSize(24, 20);
        snapBtn->setEnabled(false);
        snapBtn->setToolTip(QStringLiteral("M3에서 제공"));
        snapBtn->setStyleSheet(kToolButton);

        recBtn = new QPushButton(QStringLiteral("●"), infoBar);
        recBtn->setFixedSize(24, 20);
        recBtn->setEnabled(false);
        recBtn->setToolTip(QStringLiteral("M3에서 제공"));
        recBtn->setStyleSheet(kToolButton);

        auto* barRow = new QHBoxLayout(infoBar);
        barRow->setContentsMargins(6, 4, 6, 4);
        barRow->setSpacing(0);
        barRow->addWidget(nameLabel);
        barRow->addStretch();
        barRow->addWidget(packetLabel);
        barRow->addWidget(snapBtn);
        barRow->addWidget(recBtn);

        // ── 영상 영역 ───────────────────────────────────────────────────
        video = new VideoTileWidget(slot, this);
        video->setMinimumSize(160, 120);

        mainLay->addWidget(infoBar);
        mainLay->addWidget(video, 1);

        setContextMenuPolicy(Qt::CustomContextMenu);
    }
};

// ─────────────────────────────────────────────────────────────────────────

GridView::GridView(nv::infra::ChannelSourceFactory* slotRegistry, Callbacks cb, QWidget* parent)
    : QWidget(parent), m_slots(slotRegistry), m_cb(std::move(cb))
{
    setStyleSheet(QStringLiteral("background-color: black;"));
    m_grid = new QGridLayout(this);
    m_grid->setSpacing(kGridSpacing);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setAlignment(Qt::AlignTop | Qt::AlignLeft);
}

// ── 셀 크기 재계산 + 모든 셀에 setFixedSize 적용 (레거시 1959~1960행) ───
void GridView::relayout()
{
    if (m_cols < 1) return;

    const int w        = width();
    const int cellW    = qMax(1, (w - (m_cols - 1) * kGridSpacing) / m_cols);
    const int cellH    = cellW * 3 / 4 + kInfoBarHeight;
    const QSize cs(cellW, cellH);

    // 타일 셀
    for (auto& [id, tile] : m_tiles) {
        if (tile) tile->setFixedSize(cs);
    }

    // 빈 셀 — QLabel (objectName "emptyCell")
    const int total = m_grid->count();
    for (int i = 0; i < total; ++i) {
        auto* item = m_grid->itemAt(i);
        if (!item) continue;
        auto* w = item->widget();
        if (w && !dynamic_cast<Tile*>(w)) {
            w->setFixedSize(cs);
        }
    }
}

void GridView::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    relayout();
}

void GridView::rebuild(const std::vector<nv::domain::ChannelConfig>& configs,
                       int manualColumns)
{
    m_lastConfigs = configs;

    // 전체 철거
    while (auto* item = m_grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_tiles.clear();

    // 이전 stretch 리셋
    const int prevRows = m_grid->rowCount();
    const int prevCols = m_grid->columnCount();
    for (int r = 0; r < prevRows; ++r) m_grid->setRowStretch(r, 0);
    for (int c = 0; c < prevCols; ++c) m_grid->setColumnStretch(c, 0);

    const int n    = static_cast<int>(configs.size());
    const int cols = manualColumns > 0 ? manualColumns : nv::domain::grid::autoColumns(n);
    const int rows = nv::domain::grid::rowsFor(n, cols);

    m_cols = cols;

    // 레거시: 균등 stretch 제거 — 위-왼쪽 정렬, 남는 공간 검정 배경
    // (setAlignment(AlignTop|AlignLeft) 이미 생성자에서 설정)

    int chanIdx = 0;
    const int totalCells = rows * cols;
    for (int cell = 0; cell < totalCells; ++cell) {
        const int r = cell / cols;
        const int c = cell % cols;

        if (chanIdx < n) {
            const auto& cfg = configs[static_cast<std::size_t>(chanIdx)];
            auto* slot = m_slots ? m_slots->slot(cfg.id) : nullptr;
            if (slot != nullptr) {
                auto* tile = new Tile(*slot, cfg.id, QString::fromStdString(cfg.name), this);
                m_grid->addWidget(tile, r, c);
                m_tiles[QString::fromStdString(cfg.id)] = tile;

                connect(tile->video, &VideoTileWidget::framePainted, this,
                        [this, id = cfg.id] { m_cb.framePainted(id); });

                connect(tile, &QWidget::customContextMenuRequested, this,
                        [this, tile](const QPoint& p) {
                    QMenu menu;
                    menu.setStyleSheet(QStringLiteral(
                        "QMenu { background-color: #ffffff; color: #1f1f1f; "
                        "border: 1px solid #c8c8c8; font-size: 12px; }"
                        "QMenu::item { padding: 6px 20px; }"
                        "QMenu::item:selected { background-color: #dbeafe; }"));
                    auto* actEdit   = menu.addAction(QStringLiteral("채널 수정"));
                    auto* actRetry  = menu.addAction(QStringLiteral("재시도"));
                    QMenu* swapMenu = menu.addMenu(QStringLiteral("위치 교환"));
                    for (const auto& other : m_lastConfigs) {
                        if (other.id == tile->channelId) continue;
                        swapMenu->addAction(QString::fromStdString(other.name))->setData(
                            QString::fromStdString(other.id));
                    }
                    auto* actRemove = menu.addAction(QStringLiteral("채널 삭제"));
                    auto* chosen    = menu.exec(tile->mapToGlobal(p));
                    if (chosen == actEdit)        m_cb.editRequested(tile->channelId);
                    else if (chosen == actRetry)  m_cb.retryRequested(tile->channelId);
                    else if (chosen == actRemove) m_cb.removeRequested(tile->channelId);
                    else if (chosen != nullptr && !chosen->data().isNull())
                        m_cb.swapRequested(tile->channelId,
                                           chosen->data().toString().toStdString());
                });

                ++chanIdx;
                continue;
            }
        }

        // 빈 셀 (레거시 GRID_CELL_STYLE: #ededed, "No Stream" #777 14px)
        auto* empty = new QLabel(QStringLiteral("No Stream"), this);
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(
            QStringLiteral("color:#777; font-size:14px; background:#ededed;"));
        empty->setMinimumSize(160, 120);
        m_grid->addWidget(empty, r, c);

        if (chanIdx < n) ++chanIdx;  // slot이 null인 채널도 셀 하나 소비
    }

    relayout();
}

void GridView::updateTileStatus(const QString& channelId, const QString& state, int attempts,
                                const QList<int>& stages, double pps, qlonglong msSince)
{
    auto it = m_tiles.find(channelId);
    if (it == m_tiles.end()) return;
    Tile* t = it->second;
    (void)stages;

    // 채널명 (시도 횟수 병기)
    if (attempts > 0) {
        t->nameLabel->setText(
            QStringLiteral("%1  [%2 %3회]").arg(t->name, statusTextFor(state)).arg(attempts));
    } else {
        t->nameLabel->setText(
            QStringLiteral("%1  [%2]").arg(t->name, statusTextFor(state)));
    }

    // 패킷 라벨
    if (msSince < 0) {
        t->packetLabel->setText(QStringLiteral("패킷 —"));
        t->packetLabel->setStyleSheet(
            QStringLiteral("color:%1; font-size:10px; background:transparent;")
                .arg(packetColor(-1)));
    } else {
        t->packetLabel->setText(
            QStringLiteral("패킷 %1/s · %2초 전")
                .arg(pps, 0, 'f', 1)
                .arg(msSince / 1000.0, 0, 'f', 1));
        t->packetLabel->setStyleSheet(
            QStringLiteral("color:%1; font-size:10px; font-weight:bold; background:transparent;")
                .arg(QLatin1String(packetColor(msSince))));
    }
}

} // namespace nv::ui
