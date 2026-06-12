#include "GridView.h"
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
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
    if (state == QStringLiteral("Streaming"))    return QStringLiteral("연결됨");
    if (state == QStringLiteral("Connecting"))   return QStringLiteral("연결 중");
    if (state == QStringLiteral("SessionOpen"))  return QStringLiteral("연결 중");
    if (state == QStringLiteral("Reconnecting")) return QStringLiteral("재접속 중");
    if (state == QStringLiteral("Stalled"))      return QStringLiteral("재접속 중");
    if (state == QStringLiteral("Failed"))       return QStringLiteral("실패");
    if (state == QStringLiteral("Idle"))         return QStringLiteral("대기");
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
    QLabel*          stageDots   = nullptr;
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

        stageDots = new QLabel(QStringLiteral("······"), infoBar);
        stageDots->setStyleSheet(
            QStringLiteral("font-size:10px; background:transparent; padding: 0 2px;"));
        stageDots->setTextFormat(Qt::RichText);

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
        barRow->addWidget(stageDots);
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
    : QScrollArea(parent), m_slots(slotRegistry), m_cb(std::move(cb))
{
    // 레거시 스크롤 영역 설정: 수직 스크롤 항상 표시(viewport 폭 진동 방지), 수평 없음, 프레임 없음, 검정 배경
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setStyleSheet(QStringLiteral("QScrollArea { background-color: black; border: none; }"));
    setWidgetResizable(false);  // m_content 크기를 직접 제어 (레거시 setFixedHeight 방식)

    // 콘텐츠 위젯 + 그리드 레이아웃
    m_content = new QWidget();
    m_content->setStyleSheet(QStringLiteral("background-color: black;"));
    m_grid = new QGridLayout(m_content);
    m_grid->setSpacing(kGridSpacing);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    setWidget(m_content);
}

// ── 레거시 채움 알고리즘 (MainWindow.cpp updateGridCellSizes 직접 이식) ──────
// rebuild()와 resizeEvent() 양쪽에서 호출. 계산된 레이아웃이 직전과 동일하면 스킵.
void GridView::relayout()
{
    if (m_inRelayout) return;
    m_inRelayout = true;
    struct Guard { bool& flag; ~Guard() { flag = false; } } guard{m_inRelayout};

    const auto& configs       = m_lastConfigs;
    const int   manualColumns = m_lastManualColumns;

    const int n = static_cast<int>(configs.size());

    // cols
    const int cols = manualColumns > 0 ? manualColumns : nv::domain::grid::autoColumns(n);

    // maxGridIndex: configs의 gridIndex 최댓값 (없으면 -1)
    int maxGridIndex = -1;
    for (const auto& cfg : configs) {
        if (cfg.gridIndex > maxGridIndex) maxGridIndex = cfg.gridIndex;
    }

    // 뷰포트 크기 — 레거시: m_gridScrollArea->viewport()->width()/height()
    const int viewportWidth  = viewport()->width();
    const int viewportHeight = viewport()->height();

    // 셀 크기
    const int cellW    = qMax(1, (viewportWidth - (cols - 1) * kGridSpacing) / cols);
    const int leftover = viewportWidth - (cellW * cols + (cols - 1) * kGridSpacing);
    const int cellH    = cellW * 3 / 4 + kInfoBarHeight;

    // 행 수 (레거시 공식)
    const int requiredRows    = qMax(1, (maxGridIndex + cols) / cols);
    const int rowsForViewport = qMax(1, (viewportHeight + kGridSpacing + cellH) / (cellH + kGridSpacing));
    const int rows            = qMax(requiredRows, rowsForViewport);
    const int totalCells      = rows * cols;

    // ── 리사이즈 가드: 모든 파라미터가 동일하면 재구성 불필요 ────────────
    if (cols  == m_cachedCols  &&
        rows  == m_cachedRows  &&
        cellW == m_cachedCellW &&
        cellH == m_cachedCellH) {
        return;
    }

    // ── occupied 맵 구성: gridIndex → config ─────────────────────────────
    // gridIndex가 totalCells 범위 밖이면 비어있는 가장 앞 셀로 fallback
    QHash<int, const nv::domain::ChannelConfig*> occupied;

    // 1패스: 범위 안에 있는 채널 먼저 배치
    for (const auto& cfg : configs) {
        if (cfg.gridIndex >= 0 && cfg.gridIndex < totalCells) {
            occupied.insert(cfg.gridIndex, &cfg);
        }
    }

    // 2패스: 범위 밖 채널을 앞에서부터 비어있는 셀에 배치
    int fallbackCell = 0;
    for (const auto& cfg : configs) {
        if (cfg.gridIndex < 0 || cfg.gridIndex >= totalCells) {
            while (fallbackCell < totalCells && occupied.contains(fallbackCell)) {
                ++fallbackCell;
            }
            if (fallbackCell < totalCells) {
                occupied.insert(fallbackCell, &cfg);
                ++fallbackCell;
            }
        }
    }

    // ── 셀 배치 — 위젯 파괴 절대 금지; 재배치 + 크기갱신만 ───────────────
    int placeholderUsed = 0;

    for (int i = 0; i < totalCells; ++i) {
        const int r = i / cols;
        const int c = i % cols;

        const nv::domain::ChannelConfig* cfg = occupied.value(i, nullptr);

        // 마지막 열은 정수 나눗셈 나머지를 흡수해 우측 검정 띠 제거
        const int w = cellW + (c == cols - 1 ? leftover : 0);
        const QSize cellSize(w, cellH);

        if (cfg != nullptr) {
            auto* tile = m_tiles.count(QString::fromStdString(cfg->id))
                             ? m_tiles[QString::fromStdString(cfg->id)]
                             : nullptr;
            if (tile != nullptr) {
                // 레거시 패턴: 이미 해당 위치에 있지 않으면 재배치
                auto* existing = m_grid->itemAtPosition(r, c);
                if (!existing || existing->widget() != tile) {
                    m_grid->addWidget(tile, r, c);
                }
                if (tile->size() != cellSize) {
                    tile->setFixedSize(cellSize);
                }
                tile->show();
                continue;
            }
        }

        // 빈 셀 — 플레이스홀더 풀에서 재사용 (없으면 생성, delete 금지)
        QLabel* ph = nullptr;
        if (placeholderUsed < static_cast<int>(m_placeholders.size())) {
            ph = m_placeholders[placeholderUsed];
        } else {
            ph = new QLabel(QStringLiteral("No Stream"), m_content);
            ph->setAlignment(Qt::AlignCenter);
            ph->setStyleSheet(
                QStringLiteral("color:#777; font-size:14px; background:#ededed;"));
            m_placeholders.push_back(ph);
        }
        ++placeholderUsed;

        auto* existing = m_grid->itemAtPosition(r, c);
        if (!existing || existing->widget() != ph) {
            m_grid->addWidget(ph, r, c);
        }
        if (ph->size() != cellSize) {
            ph->setFixedSize(cellSize);
        }
        ph->show();
    }

    // 사용하지 않는 플레이스홀더 숨김 (delete 금지 — 풀 유지)
    for (int i = placeholderUsed; i < static_cast<int>(m_placeholders.size()); ++i) {
        m_placeholders[i]->hide();
    }

    // ── 레거시 gridHeight 방식: m_content 높이를 고정 (뷰포트 채움) ──────
    const int gridHeight = rows * cellH + (rows - 1) * kGridSpacing;
    m_content->setFixedHeight(gridHeight);
    m_content->setFixedWidth(viewportWidth);

    // ── 캐시 갱신 ────────────────────────────────────────────────────────
    m_cachedCols  = cols;
    m_cachedRows  = rows;
    m_cachedCellW = cellW;
    m_cachedCellH = cellH;
}

void GridView::resizeEvent(QResizeEvent* ev)
{
    QScrollArea::resizeEvent(ev);
    if (!m_relayoutQueued) {
        m_relayoutQueued = true;
        QTimer::singleShot(0, this, [this] {
            m_relayoutQueued = false;
            relayout();
        });
    }
}

void GridView::rebuild(const std::vector<nv::domain::ChannelConfig>& configs,
                       int manualColumns)
{
    // 파라미터 저장 — resizeEvent에서 재호출 시 사용
    m_lastConfigs       = configs;
    m_lastManualColumns = manualColumns;

    // ── diff: configs에 없는 기존 타일 삭제 ─────────────────────────────
    QSet<QString> newIds;
    for (const auto& cfg : configs) {
        newIds.insert(QString::fromStdString(cfg.id));
    }
    for (auto it = m_tiles.begin(); it != m_tiles.end(); ) {
        if (!newIds.contains(it->first)) {
            delete it->second;
            it = m_tiles.erase(it);
        } else {
            ++it;
        }
    }

    // ── diff: 새 채널 타일 생성 + 기존 타일 이름 라벨 갱신 ──────────────
    for (const auto& cfg : configs) {
        const QString qId   = QString::fromStdString(cfg.id);
        const QString qName = QString::fromStdString(cfg.name);

        auto tileIt = m_tiles.find(qId);
        if (tileIt != m_tiles.end()) {
            // 기존 타일: 이름만 갱신 (파괴 금지)
            tileIt->second->name = qName;
            tileIt->second->nameLabel->setText(qName);
        } else {
            // 신규 타일 생성
            auto* slot = m_slots ? m_slots->slot(cfg.id) : nullptr;
            if (slot == nullptr) continue;

            auto* tile = new Tile(*slot, cfg.id, qName, m_content);
            m_tiles[qId] = tile;

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
        }
    }

    // 캐시를 무효화해 relayout()이 반드시 재배치하도록 강제
    m_cachedCols  = 0;
    m_cachedRows  = 0;
    m_cachedCellW = 0;
    m_cachedCellH = 0;

    relayout();
}

void GridView::updateTileStatus(const QString& channelId, const QString& state, int attempts,
                                const QList<int>& stages, double pps, qlonglong msSince,
                                const QString& reason)
{
    auto it = m_tiles.find(channelId);
    if (it == m_tiles.end()) return;
    Tile* t = it->second;

    // ── 6단계 점 인디케이터 (StageState: 0=Unknown, 1=Ok, 2=Failed, 3=NotApplicable)
    static const char* kStageNames[] = {"장비도달", "Relay수신", "RTSP세션", "패킷수신", "디코딩", "표시"};
    QString dots;
    QString tooltip;
    for (int i = 0; i < stages.size() && i < 6; ++i) {
        const int s = stages[i];
        if (s == 3) {
            dots += QStringLiteral("<span style='color:#bbb'>–</span>");
            tooltip += QStringLiteral("%1: 해당없음\n").arg(QLatin1String(kStageNames[i]));
        } else {
            const char* color = (s == 1) ? "#12823b" : (s == 2) ? "#d13438" : "#999";
            dots += QStringLiteral("<span style='color:%1'>●</span>").arg(QLatin1String(color));
            const QString stateStr = (s == 1) ? QStringLiteral("정상")
                                   : (s == 2) ? QStringLiteral("실패")
                                              : QStringLiteral("알수없음");
            if (s == 2 && reason != QStringLiteral("None") && !reason.isEmpty()) {
                tooltip += QStringLiteral("%1: %2(%3)\n").arg(QLatin1String(kStageNames[i]), stateStr, reason);
            } else {
                tooltip += QStringLiteral("%1: %2\n").arg(QLatin1String(kStageNames[i]), stateStr);
            }
        }
    }
    t->stageDots->setText(dots);
    t->stageDots->setToolTip(tooltip.trimmed());

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
