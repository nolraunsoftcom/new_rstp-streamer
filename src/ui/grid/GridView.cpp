#include "GridView.h"
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>
#include "src/domain/layout/GridRules.h"
#include "src/infra/ffmpeg/ChannelSourceFactory.h"
#include "src/infra/video/LatestFrameSlot.h"
#include "src/ui/grid/VideoTileWidget.h"

namespace nv::ui {

// Forward-declare the nested Tile struct body here so GridView constructor can reference it
struct GridView::Tile : public QWidget {
    QLabel* header = nullptr;
    QLabel* flow = nullptr;
    VideoTileWidget* video = nullptr;
    std::string channelId;
    QString name;

    Tile(nv::infra::LatestFrameSlot& slot, std::string id, QString nm, QWidget* parent)
        : QWidget(parent), channelId(std::move(id)), name(std::move(nm)) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(2, 2, 2, 2);
        lay->setSpacing(2);
        header = new QLabel(name, this);
        header->setStyleSheet(QStringLiteral(
            "color: #eeeeee; font-weight: bold; font-size: 12px; "
            "background: rgba(0,0,0,120); padding: 2px 4px;"));
        video = new VideoTileWidget(slot, this);
        video->setMinimumSize(160, 120);
        flow = new QLabel(QStringLiteral("패킷 —"), this);
        flow->setStyleSheet(QStringLiteral("color: gray; font-size: 11px; "
                                           "background: rgba(0,0,0,80); padding: 1px 4px;"));
        lay->addWidget(header);
        lay->addWidget(video, 1);
        lay->addWidget(flow);
        setContextMenuPolicy(Qt::CustomContextMenu);
        setStyleSheet(QStringLiteral("background-color: black;"));
    }
};

GridView::GridView(nv::infra::ChannelSourceFactory* slots, Callbacks cb, QWidget* parent)
    : QWidget(parent), m_slots(slots), m_cb(std::move(cb)) {
    setStyleSheet(QStringLiteral("background-color: black;"));
    m_grid = new QGridLayout(this);
    m_grid->setSpacing(1);
    m_grid->setContentsMargins(0, 0, 0, 0);
}

void GridView::rebuild(const std::vector<nv::domain::ChannelConfig>& configs,
                       int manualColumns) {
    m_lastConfigs = configs;
    // 전체 철거 — 슬롯은 factory가 보관하므로 타일 삭제는 안전
    while (auto* item = m_grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_tiles.clear();

    const int n = static_cast<int>(configs.size());
    const int cols = manualColumns > 0 ? manualColumns : nv::domain::grid::autoColumns(n);

    int cell = 0;
    for (const auto& cfg : configs) {
        auto* slot = m_slots ? m_slots->slot(cfg.id) : nullptr;
        if (slot == nullptr) continue;
        auto* tile = new Tile(*slot, cfg.id, QString::fromStdString(cfg.name), this);
        m_grid->addWidget(tile, cell / cols, cell % cols);
        m_tiles[QString::fromStdString(cfg.id)] = tile;
        ++cell;

        connect(tile->video, &VideoTileWidget::framePainted, this,
                [this, id = cfg.id] { m_cb.framePainted(id); });
        connect(tile, &QWidget::customContextMenuRequested, this, [this, tile](const QPoint& p) {
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
                m_cb.swapRequested(tile->channelId, chosen->data().toString().toStdString());
        });
    }

    if (n == 0) {
        auto* empty = new QLabel(
            QStringLiteral("채널이 없습니다 — [채널 추가]를 누르세요"), this);
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(
            QStringLiteral("color: #aaaaaa; font-size: 14px; background: black;"));
        m_grid->addWidget(empty, 0, 0);
    }
}

void GridView::updateTileStatus(const QString& channelId, const QString& state, int attempts,
                                const QList<int>& stages, double pps, qlonglong msSince) {
    auto it = m_tiles.find(channelId);
    if (it == m_tiles.end()) return;
    Tile* t = it->second;
    (void)stages;
    t->header->setText(
        QStringLiteral("%1  [%2%3]")
            .arg(t->name, state,
                 attempts > 0 ? QStringLiteral(" %1회").arg(attempts) : QString()));
    if (msSince < 0) {
        t->flow->setText(QStringLiteral("패킷 —"));
        t->flow->setStyleSheet(QStringLiteral(
            "color: gray; font-size: 11px; background: rgba(0,0,0,80); padding: 1px 4px;"));
    } else {
        t->flow->setText(QStringLiteral("패킷 %1/s · %2초 전")
                             .arg(pps, 0, 'f', 1)
                             .arg(msSince / 1000.0, 0, 'f', 1));
        const char* color =
            (msSince < 1000) ? "limegreen" : (msSince < 3000) ? "orange" : "red";
        t->flow->setStyleSheet(
            QStringLiteral("color: %1; font-weight: bold; font-size: 11px; "
                           "background: rgba(0,0,0,80); padding: 1px 4px;")
                .arg(color));
    }
}

} // namespace nv::ui
