#pragma once
#include <QScrollArea>
#include <functional>
#include <map>
#include "src/domain/channel/ChannelConfig.h"

class QGridLayout;
class QWidget;

namespace nv::infra { class ChannelSourceFactory; }

namespace nv::ui {
class VideoTileWidget;

// 채널 타일 그리드. rebuild()로 전체 재구성 (M2a 단순화 — 드래그 스왑은 우클릭 메뉴로 대체).
// 컬럼 0 = Auto. 레거시 구조: QScrollArea(수직 스크롤) + m_content(QGridLayout).
class GridView : public QScrollArea {
    Q_OBJECT
public:
    struct Callbacks {
        std::function<void(std::string id)> editRequested;
        std::function<void(std::string id)> removeRequested;
        std::function<void(std::string id)> retryRequested;
        std::function<void(std::string id)> framePainted;
        std::function<void(std::string idA, std::string idB)> swapRequested;
    };

    GridView(nv::infra::ChannelSourceFactory* slotRegistry, Callbacks cb, QWidget* parent = nullptr);

    void rebuild(const std::vector<nv::domain::ChannelConfig>& configs, int manualColumns);
    void updateTileStatus(const QString& channelId, const QString& state, int attempts,
                          const QList<int>& stages, double pps, qlonglong msSince);

protected:
    void resizeEvent(QResizeEvent* ev) override;

private:
    // Rebuilds with stored configs/manualColumns; resize guard skips if layout unchanged.
    void relayout();

    nv::infra::ChannelSourceFactory* m_slots = nullptr;
    Callbacks m_cb;
    QWidget*     m_content = nullptr;   // scroll content widget
    QGridLayout* m_grid    = nullptr;
    struct Tile;
    std::map<QString, Tile*> m_tiles;             // channelId → tile
    std::vector<nv::domain::ChannelConfig> m_lastConfigs;
    int m_lastManualColumns = 0;

    // Resize guard: cached layout parameters
    int m_cachedCols     = 0;
    int m_cachedRows     = 0;
    int m_cachedCellW    = 0;
    int m_cachedCellH    = 0;
};

} // namespace nv::ui
