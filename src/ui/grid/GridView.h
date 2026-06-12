#pragma once
#include <QWidget>
#include <functional>
#include <map>
#include "src/domain/channel/ChannelConfig.h"

class QGridLayout;

namespace nv::infra { class ChannelSourceFactory; }

namespace nv::ui {
class VideoTileWidget;

// 채널 타일 그리드. rebuild()로 전체 재구성 (M2a 단순화 — 드래그 스왑은 우클릭 메뉴로 대체).
// 컬럼 0 = Auto.
class GridView : public QWidget {
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

private:
    nv::infra::ChannelSourceFactory* m_slots = nullptr;
    Callbacks m_cb;
    QGridLayout* m_grid = nullptr;
    struct Tile;
    std::map<QString, Tile*> m_tiles;             // channelId → tile
    std::vector<nv::domain::ChannelConfig> m_lastConfigs;
};

} // namespace nv::ui
