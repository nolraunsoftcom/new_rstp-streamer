#pragma once
#include <QDialog>
#include <QHash>
#include <functional>
#include "src/domain/channel/ChannelConfig.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;

namespace nv::ui {

// 채널 정보 다이얼로그 — 레거시 ChannelInfoDialog(통계 트리) 미러.
// 레거시는 libVLC 미디어 통계였으나 우리 FFmpeg 파이프라인엔 일부 통계가 없어,
// 산출 가능한 값(디코드/표시/손실/FPS/수신바이트/비트레이트)만 채우고 나머지는 0으로 둔다
// (레거시도 오디오·스트림출력 등은 0으로 표시됨). 1초 주기로 provider를 호출해 라이브 갱신.
class ChannelInfoDialog : public QDialog {
    Q_OBJECT
public:
    // provider가 반환하는 실시간 통계(MainWindow 캐시에서 채움).
    struct Stats {
        bool streaming = false;
        long long decodedFrames = 0;
        long long displayedFrames = 0;
        long long droppedFrames = 0;
        double outputFps = 0.0;       // packets/sec ≈ frame rate
        long long readBytesTotal = 0;
        double bitrateKbps = 0.0;
    };

    using StatsProvider = std::function<Stats()>;

    ChannelInfoDialog(const nv::domain::ChannelConfig& cfg,
                      StatsProvider provider,
                      QWidget* parent = nullptr);

private:
    QTreeWidgetItem* addGroup(const QString& title);
    void addMetric(QTreeWidgetItem* group, const QString& key, const QString& label);
    void refresh();

    nv::domain::ChannelConfig m_cfg;
    StatsProvider m_provider;
    QTreeWidget* m_tree = nullptr;
    QHash<QString, QTreeWidgetItem*> m_items;   // key → 값 아이템
    QLabel* m_playUrl = nullptr;
    QLabel* m_sourceUrl = nullptr;
    QLabel* m_relayLabel = nullptr;
};

} // namespace nv::ui
