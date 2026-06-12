#pragma once
#include <QLabel>
#include <QScrollArea>
#include <QTimer>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "src/domain/channel/ChannelConfig.h"
#include "src/ui/shell/RepaintClock.h"

class QGridLayout;
class QWidget;

namespace nv::infra { class ChannelSourceFactory; }

namespace nv::ui {
class VideoTileWidget;

// 채널 타일 그리드. rebuild()로 채널 목록 변경 시 diff 갱신, relayout()으로 배치만 갱신.
// 타일은 영속 — 리사이즈 시 파괴 없음. 플레이스홀더는 hide/show 풀 방식.
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

    GridView(nv::infra::ChannelSourceFactory* slotRegistry, Callbacks cb,
             RepaintClock& repaintClock, QWidget* parent = nullptr);

    // 채널 목록 변경 시 호출: diff 기반 타일 추가/삭제 후 relayout()
    void rebuild(const std::vector<nv::domain::ChannelConfig>& configs, int manualColumns);
    void updateTileStatus(const QString& channelId, const QString& state, int attempts,
                          const QList<int>& stages, double pps, qlonglong msSince,
                          const QString& reason);

protected:
    void resizeEvent(QResizeEvent* ev) override;

private:
    // 배치만 갱신 — 위젯 파괴 절대 금지
    void relayout();

    nv::infra::ChannelSourceFactory* m_slots = nullptr;
    Callbacks m_cb;
    RepaintClock& m_repaintClock;
    QWidget*     m_content = nullptr;   // scroll content widget
    QGridLayout* m_grid    = nullptr;
    struct Tile;
    std::map<std::string, Tile*> m_tiles;         // channelId → tile (영속, 부채 #13: std::string 키)
    std::vector<QLabel*>     m_placeholders;      // 빈 셀 플레이스홀더 풀 (hide/show)
    std::vector<nv::domain::ChannelConfig> m_lastConfigs;
    int m_lastManualColumns = 0;

    // Resize guard: cached layout parameters
    int m_cachedCols     = 0;
    int m_cachedRows     = 0;
    int m_cachedCellW    = 0;
    int m_cachedCellH    = 0;

    // Oscillation fix: re-entrancy guard + resize coalescing
    bool m_inRelayout      = false;
    bool m_relayoutQueued  = false;
};

} // namespace nv::ui
