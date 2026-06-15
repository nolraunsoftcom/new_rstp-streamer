#include "GridView.h"
#include <algorithm>
#include <set>
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QVBoxLayout>
#include "src/domain/layout/GridRules.h"
#include "src/domain/recording/RecordingState.h"
#include "src/ui/grid/VideoTileWidget.h"
#include "src/ui/common/Confirm.h"
#include "src/ui/common/Style.h"

namespace nv::ui {

// 그리드 타일 DnD MIME — 페이로드는 드래그 출발 타일의 channelId(UTF-8).
static const QString kTileDragMime = QStringLiteral("application/x-nv-channel-id");

// ── 레거시 상수 (MainWindow.cpp 기준) ─────────────────────────────────────
static constexpr int kInfoBarHeight = 28;   // VIEWER_INFO_BAR_HEIGHT
static constexpr int kGridSpacing   = 1;    // GRID_SPACING

// ── 레거시 Style.h TOOL_BUTTON — Style.h 중앙화 상수 사용 ─────────────────
// nv::ui::style::TOOL_BUTTON 및 TOOL_BUTTON_REC 참조

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
    QLabel*          recBadge    = nullptr;   // M3-5: "REC" 빨강 뱃지
    VideoTileWidget* video       = nullptr;
    std::string      channelId;
    QString          name;
    nv::domain::RecordingState recState{nv::domain::RecordingState::Idle};  // 메뉴 라벨(녹화 시작/중지)용

    // ── DnD(위치 교환) — 타일은 드래그 출발지. 드롭은 GridView(콘텐츠)가 위치 기반 처리. ──
    QPoint           dragStartPos;            // 좌클릭 누른 지점(드래그 임계 판정용)
    std::function<void()> onDoubleClick;      // 더블클릭 → 전체화면(GridView가 배선)

    Tile(nv::app::IFrameSurfaceRegistry& registry, std::string id, QString nm, QWidget* parent)
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
        nameLabel->setTextFormat(Qt::PlainText);   // S3: 채널명 HTML 인젝션 방지
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
        snapBtn->setEnabled(true);
        snapBtn->setToolTip(QStringLiteral("스냅샷 저장"));
        snapBtn->setStyleSheet(nv::ui::style::TOOL_BUTTON);

        recBtn = new QPushButton(QStringLiteral("●"), infoBar);
        recBtn->setFixedSize(24, 20);
        recBtn->setEnabled(true);
        recBtn->setToolTip(QStringLiteral("녹화 시작/중지"));
        recBtn->setStyleSheet(nv::ui::style::TOOL_BUTTON);

        recBadge = new QLabel(QStringLiteral("REC"), infoBar);
        recBadge->setStyleSheet(QStringLiteral(
            "color:#fff; background:#d13438; font-size:9px; font-weight:bold; "
            "padding: 1px 3px; border-radius:2px;"));
        recBadge->hide();

        auto* barRow = new QHBoxLayout(infoBar);
        barRow->setContentsMargins(6, 4, 6, 4);
        barRow->setSpacing(0);
        barRow->addWidget(nameLabel);
        barRow->addStretch();
        barRow->addWidget(recBadge);
        barRow->addWidget(stageDots);
        barRow->addWidget(packetLabel);
        barRow->addWidget(snapBtn);
        barRow->addWidget(recBtn);

        // ── 영상 영역 ───────────────────────────────────────────────────
        video = new VideoTileWidget(registry, channelId, this);
        video->setMinimumSize(160, 120);

        mainLay->addWidget(infoBar);
        mainLay->addWidget(video, 1);

        setContextMenuPolicy(Qt::CustomContextMenu);

        // ── DnD: 타일을 드래그 출발지로. 영상 영역(가장 큰 면적)만 마우스 투명 처리해
        // 그 위의 누름/이동 이벤트가 타일(this)에 도달하게 한다 → 영상에서 드래그 시작.
        // 정보바는 투명하게 두지 않는다(스냅샷/녹화 버튼 클릭 보존). 정보바 위 컨텍스트 메뉴는
        // 정보바가 contextMenuEvent를 처리하지 않아 타일로 전파된다. 드롭은 GridView(콘텐츠)가
        // 위치 기반으로 처리하므로 타일은 setAcceptDrops를 켜지 않는다(빈칸 드롭도 동작). ──
        video->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) dragStartPos = e->pos();
        QWidget::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) { QWidget::mouseMoveEvent(e); return; }
        if ((e->pos() - dragStartPos).manhattanLength() < QApplication::startDragDistance())
            return;
        auto* drag = new QDrag(this);
        auto* mime = new QMimeData();
        mime->setData(kTileDragMime, QByteArray::fromStdString(channelId));
        mime->setText(name);
        drag->setMimeData(mime);
        drag->exec(Qt::MoveAction);
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && onDoubleClick) { onDoubleClick(); return; }
        QWidget::mouseDoubleClickEvent(e);
    }
};

// ─────────────────────────────────────────────────────────────────────────

GridView::GridView(nv::app::IFrameSurfaceRegistry* registry, Callbacks cb,
                   RepaintClock& repaintClock, QWidget* parent)
    : QScrollArea(parent), m_registry(registry), m_cb(std::move(cb)),
      m_repaintClock(repaintClock)
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

    // ── DnD: 콘텐츠에서 위치 기반 드롭 처리(빈칸 이동/점유 교환). 타일은 드래그 출발지. ──
    m_content->setAcceptDrops(true);
    m_content->installEventFilter(this);
    m_dropHighlight = new QWidget(m_content);
    m_dropHighlight->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_dropHighlight->setStyleSheet(QStringLiteral(
        "background-color: rgba(0,120,212,40); border: 2px solid #0078d4;"));
    m_dropHighlight->hide();
}

int GridView::cellIndexAt(const QPoint& pos) const {
    const int cols = m_cachedCols;
    const int cw   = m_cachedCellW;
    const int ch   = m_cachedCellH;
    if (cols <= 0 || cw <= 0 || ch <= 0) return -1;
    if (pos.x() < 0 || pos.y() < 0) return -1;
    const int strideX = cw + kGridSpacing;
    const int strideY = ch + kGridSpacing;
    const int col = pos.x() / strideX;
    const int row = pos.y() / strideY;
    if (col >= cols) return -1;
    return row * cols + col;
}

bool GridView::eventFilter(QObject* obj, QEvent* ev) {
    if (obj != m_content) return QScrollArea::eventFilter(obj, ev);

    auto showHighlightAt = [this](int index) {
        if (!m_dropHighlight || index < 0 || m_cachedCols <= 0) { if (m_dropHighlight) m_dropHighlight->hide(); return; }
        const int col = index % m_cachedCols;
        const int row = index / m_cachedCols;
        const int x = col * (m_cachedCellW + kGridSpacing);
        const int y = row * (m_cachedCellH + kGridSpacing);
        m_dropHighlight->setGeometry(x, y, m_cachedCellW, m_cachedCellH);
        m_dropHighlight->raise();
        m_dropHighlight->show();
    };

    switch (ev->type()) {
    case QEvent::DragEnter: {
        auto* e = static_cast<QDragEnterEvent*>(ev);
        if (!e->mimeData()->hasFormat(kTileDragMime)) { e->ignore(); return true; }
        showHighlightAt(cellIndexAt(e->position().toPoint()));
        e->acceptProposedAction();
        return true;
    }
    case QEvent::DragMove: {
        auto* e = static_cast<QDragMoveEvent*>(ev);
        if (!e->mimeData()->hasFormat(kTileDragMime)) { e->ignore(); return true; }
        showHighlightAt(cellIndexAt(e->position().toPoint()));
        e->acceptProposedAction();
        return true;
    }
    case QEvent::DragLeave: {
        if (m_dropHighlight) m_dropHighlight->hide();
        return true;
    }
    case QEvent::Drop: {
        auto* e = static_cast<QDropEvent*>(ev);
        if (m_dropHighlight) m_dropHighlight->hide();
        const auto src = e->mimeData()->data(kTileDragMime).toStdString();
        const int index = cellIndexAt(e->position().toPoint());
        if (src.empty() || index < 0) { e->ignore(); return true; }
        if (m_cb.moveRequested) m_cb.moveRequested(src, index);
        e->acceptProposedAction();
        return true;
    }
    default:
        break;
    }
    return QScrollArea::eventFilter(obj, ev);
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
    const int leftover = std::max(0, viewportWidth - (cellW * cols + (cols - 1) * kGridSpacing));  // F2: 음수 방지
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

    // ── F1: 재배치 시작 전 모든 아이템을 레이아웃에서 detach — 위젯은 보존, QLayoutItem만 삭제
    // itemAtPosition 분기 없이 매번 깨끗이 재구성 → 스왑/삭제 후 기하 충돌 원천 차단
    while (QLayoutItem* it = m_grid->takeAt(0)) { delete it; }

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
            auto* tile = m_tiles.count(cfg->id) ? m_tiles[cfg->id] : nullptr;
            if (tile != nullptr) {
                m_grid->addWidget(tile, r, c);  // detach 후 무조건 1회 등록 — 이중등록 불가
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

        m_grid->addWidget(ph, r, c);  // detach 후 무조건 1회 등록
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
    std::set<std::string> newIds;
    for (const auto& cfg : configs) {
        newIds.insert(cfg.id);
    }
    for (auto it = m_tiles.begin(); it != m_tiles.end(); ) {
        if (newIds.find(it->first) == newIds.end()) {
            delete it->second;
            it = m_tiles.erase(it);
        } else {
            ++it;
        }
    }

    // ── diff: 새 채널 타일 생성 + 기존 타일 이름 라벨 갱신 ──────────────
    for (const auto& cfg : configs) {
        const QString qName = QString::fromStdString(cfg.name);

        auto tileIt = m_tiles.find(cfg.id);
        if (tileIt != m_tiles.end()) {
            // 기존 타일: 이름만 갱신 (파괴 금지)
            tileIt->second->name = qName;
            tileIt->second->nameLabel->setText(qName);
        } else {
            // 신규 타일 생성
            if (m_registry == nullptr) continue;

            auto* tile = new Tile(*m_registry, cfg.id, qName, m_content);
            m_tiles[cfg.id] = tile;

            // 더블클릭 → 전체화면 탭
            tile->onDoubleClick = [this, id = cfg.id] {
                if (m_cb.fullscreenRequested) m_cb.fullscreenRequested(id);
            };

            // 단일 RepaintClock tick에 pollFrame 연결 (자체 타이머 없음)
            connect(&m_repaintClock, &RepaintClock::tick,
                    tile->video, &VideoTileWidget::pollFrame);

            connect(tile->video, &VideoTileWidget::framePainted, this,
                    [this, id = cfg.id] { m_cb.framePainted(id); });

            // M3-5: 📷 스냅샷 버튼 — control 스레드로 위임
            if (m_cb.snapshotRequested) {
                connect(tile->snapBtn, &QPushButton::clicked, this,
                        [this, id = cfg.id] { m_cb.snapshotRequested(id); });
            }
            // M3-5: ● 녹화 토글 버튼 — control 스레드로 위임
            if (m_cb.recordToggleRequested) {
                connect(tile->recBtn, &QPushButton::clicked, this,
                        [this, id = cfg.id] { m_cb.recordToggleRequested(id); });
            }

            connect(tile, &QWidget::customContextMenuRequested, this,
                    [this, tile](const QPoint& p) {
                QMenu menu;
                menu.setStyleSheet(QStringLiteral(
                    "QMenu { background-color: #ffffff; color: #1f1f1f; "
                    "border: 1px solid #c8c8c8; font-size: 12px; }"
                    "QMenu::item { padding: 6px 20px; }"
                    "QMenu::item:selected { background-color: #dbeafe; }"));
                // 레거시 VlcWidget::showContextMenu 구성 그대로(위치 교환은 DnD로 대체돼 제외).
                using nv::domain::RecordingState;
                const bool rec = (tile->recState == RecordingState::Recording ||
                                  tile->recState == RecordingState::Starting);
                auto* actFull   = menu.addAction(QStringLiteral("전체화면으로 열기"));
                auto* actInfo   = menu.addAction(QStringLiteral("채널 정보"));
                auto* actEdit   = menu.addAction(QStringLiteral("채널 수정"));
                auto* actSnap   = menu.addAction(QStringLiteral("스냅샷 저장"));
                auto* actRecord = menu.addAction(rec ? QStringLiteral("녹화 중지")
                                                     : QStringLiteral("녹화 시작"));
                menu.addSeparator();
                auto* actRetry  = menu.addAction(QStringLiteral("재연결"));
                menu.addSeparator();
                auto* actRemove = menu.addAction(QStringLiteral("채널 삭제"));
                auto* chosen    = menu.exec(tile->mapToGlobal(p));
                if (chosen == nullptr) return;
                if (chosen == actFull) {
                    if (m_cb.fullscreenRequested) m_cb.fullscreenRequested(tile->channelId);
                }
                else if (chosen == actInfo) {
                    if (m_cb.infoRequested) m_cb.infoRequested(tile->channelId);
                }
                else if (chosen == actEdit)   m_cb.editRequested(tile->channelId);
                else if (chosen == actSnap) {
                    if (m_cb.snapshotRequested) m_cb.snapshotRequested(tile->channelId);
                }
                else if (chosen == actRecord) {
                    if (m_cb.recordToggleRequested) m_cb.recordToggleRequested(tile->channelId);
                }
                else if (chosen == actRetry)  m_cb.retryRequested(tile->channelId);
                else if (chosen == actRemove) {
                    // U1: 삭제 확인 다이얼로그 (F5: confirmDelete 헬퍼)
                    // parent는 최상위 윈도우 — tile(RHI 영상 표면)을 parent로 주면 macOS에서
                    // 메시지박스가 깨져 렌더된다(리스트 삭제와 동일하게 윈도우 기준으로 띄움).
                    if (nv::ui::confirmDelete(tile->window(), tile->name))
                        m_cb.removeRequested(tile->channelId);
                }
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
    auto it = m_tiles.find(channelId.toStdString());
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

void GridView::updateRecordingState(const QString& channelId, nv::domain::RecordingState state)
{
    auto it = m_tiles.find(channelId.toStdString());
    if (it == m_tiles.end()) return;
    Tile* t = it->second;
    t->recState = state;   // 우클릭 메뉴 "녹화 시작/중지" 라벨용

    using nv::domain::RecordingState;
    const bool active = (state == RecordingState::Recording ||
                         state == RecordingState::Starting);

    // ● 버튼: Starting/Recording이면 TOOL_BUTTON_REC(#ff4040), 아니면 TOOL_BUTTON(기본)
    t->recBtn->setStyleSheet(active ? nv::ui::style::TOOL_BUTTON_REC
                                    : nv::ui::style::TOOL_BUTTON);
    t->recBtn->setToolTip(active ? QStringLiteral("녹화 중지") : QStringLiteral("녹화 시작"));

    // P4d: REC 뱃지 — Starting(노랑), Recording(빨강), Idle(숨김)
    if (state == RecordingState::Starting) {
        t->recBadge->setStyleSheet(nv::ui::style::REC_BADGE_STARTING);
        t->recBadge->show();
    } else if (state == RecordingState::Recording) {
        t->recBadge->setStyleSheet(nv::ui::style::REC_BADGE_ACTIVE);
        t->recBadge->show();
    } else {
        t->recBadge->hide();
    }
}

} // namespace nv::ui
