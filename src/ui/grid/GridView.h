#pragma once
#include <QLabel>
#include <QScrollArea>
#include <QTimer>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "src/app/ports/IFrameSurfaceRegistry.h"
#include "src/domain/channel/ChannelConfig.h"
#include "src/domain/recording/RecordingState.h"
#include "src/ui/shell/RepaintClock.h"

class QGridLayout;
class QWidget;

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
        // DnD 위치 이동: 타일을 그리드 위치(index)로 드롭. 점유 셀이면 swap, 빈칸이면 이동.
        std::function<void(std::string id, int targetGridIndex)> moveRequested;
        // M3-5: 정보바 버튼 콜백
        std::function<void(std::string id)> snapshotRequested;
        std::function<void(std::string id)> recordToggleRequested;
    };

    GridView(nv::app::IFrameSurfaceRegistry* registry, Callbacks cb,
             RepaintClock& repaintClock, QWidget* parent = nullptr);

    // 채널 목록 변경 시 호출: diff 기반 타일 추가/삭제 후 relayout()
    void rebuild(const std::vector<nv::domain::ChannelConfig>& configs, int manualColumns);
    void updateTileStatus(const QString& channelId, const QString& state, int attempts,
                          const QList<int>& stages, double pps, qlonglong msSince,
                          const QString& reason);
    // M3-5: 녹화 상태 변경 → 정보바 ● 버튼 + REC 뱃지 갱신
    void updateRecordingState(const QString& channelId, nv::domain::RecordingState state);

protected:
    void resizeEvent(QResizeEvent* ev) override;
    // m_content의 드래그/드롭 이벤트를 가로채 위치 기반 위치이동/교환을 처리한다.
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    // 배치만 갱신 — 위젯 파괴 절대 금지
    void relayout();
    // m_content 좌표 → 그리드 셀 index(캐시된 레이아웃 기준). 범위 밖이면 -1.
    int cellIndexAt(const QPoint& pos) const;

    nv::app::IFrameSurfaceRegistry* m_registry = nullptr;
    Callbacks m_cb;
    RepaintClock& m_repaintClock;
    QWidget*     m_content = nullptr;   // scroll content widget
    QGridLayout* m_grid    = nullptr;
    QWidget*     m_dropHighlight = nullptr;   // DnD 드롭 대상 셀 하이라이트(반투명, 평소 숨김)
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
