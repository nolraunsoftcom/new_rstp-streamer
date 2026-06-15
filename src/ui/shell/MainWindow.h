#pragma once
#include <QMainWindow>
#include <QMap>
#include <QVector>
#include <functional>
#include <memory>
#include <vector>
#include "src/domain/channel/ChannelConfig.h"
#include "src/domain/recording/RecordingState.h"
#include "src/infra/system/ResourceMonitor.h"
#include "src/ui/shell/Toast.h"

class QComboBox;
class QButtonGroup;
class QLabel;
class QTabWidget;

namespace nv::ui {
class GridView;
class ChannelListPanel;
class LogPanel;
class FilePanel;

// 레거시 패리티 3패널 셸:
//   좌측 ChannelListPanel + 토글 | 중앙 QTabWidget(전체/GridView, 검정 배경) | 토글 + 우측 패널(설정/파일/로그)
// 상태바: 채널 집계 + CPU/메모리 (ResourceMonitor 1초 주기)
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    struct Commands {
        std::function<void(std::string name, std::string url, bool autoConnect, bool useRelay)> addChannel;
        std::function<void(std::string id, std::string name, std::string url, bool autoConnect, bool useRelay)> updateChannel;
        std::function<void(std::string id)> removeChannel;
        std::function<void(std::string id)> retryChannel;
        std::function<void(std::string id)> framePainted;
        std::function<void(std::string a, std::string b)> swapChannels;   // 그리드 위치 교환(gridIndex)
        // 전체화면 탭용 영상 위젯 팩토리: channelId에 바인딩된(공유 프레임 레지스트리 기반)
        // 큰 영상 위젯을 생성해 반환. MainWindow가 탭 수명(추가/전환/닫기)을 관리한다.
        std::function<QWidget*(std::string id)> makeFullscreenView;
    };

    MainWindow(GridView* grid, ChannelListPanel* channelPanel, LogPanel* logPanel,
               Commands commands);

    int manualColumns() const;  // 0 = Auto

    // public so main.cpp gridCb.editRequested can call it
    void openEditDialog(const std::string& id);

    // 전체화면 탭 열기(레거시 openFullscreenTab): 이미 열려있으면 전환, 아니면 새 탭 추가.
    // GridView/ChannelListPanel 더블클릭·우클릭에서 호출(UI 스레드).
    void openFullscreenTab(const std::string& id);

    // 채널 정보 다이얼로그 열기(우클릭 메뉴). 현재 UI 캐시(구성/연결/녹화) 기반.
    void openChannelInfo(const std::string& id);

public slots:
    void onChannelList(QVector<QString> ids, QVector<QString> names, QVector<QString> urls,
                       QVector<int> gridIndexes, QVector<int> listIndexes,
                       QVector<bool> autoConnects, QVector<bool> useRelays);
    void onSnapshot(QString channelId, QString state, int attempts, QList<int> stages,
                    double pps, qlonglong msSinceLastPacket, QString reason,
                    double bitrateKbps, qlonglong droppedFrames,
                    qlonglong decodedFrames, qlonglong displayedFrames,
                    qlonglong readBytesTotal);
    // M3-5: 녹화 상태 변경 → 그리드 타일 버튼 + 파일 패널 갱신
    void onRecordingState(QString channelId, nv::domain::RecordingState state);
    // P3: 토스트 트리거 슬롯 — control→UI queued 호출 (기존 이벤트 흐름 재사용)
    void onSnapshotSaved(QString channelName, QString filePath);
    void onRecordingSaved(QString channelName, QString filePath, bool autoSaved,
                          qint64 bytes, int durationSec);
    void onRecordingFailed(QString channelName, QString reason);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuildGrid();
    void closeFullscreenTab(int index);   // 전체화면 탭 닫기(인덱스 0=그리드는 무시)
    void openAddDialog();
    void setLeftPanelVisible(bool visible);
    void setRightPanelVisible(bool visible);
    void updateToggleButtons();

    Commands m_commands;
    GridView* m_grid = nullptr;
    ChannelListPanel* m_channelPanel = nullptr;
    LogPanel* m_logPanel = nullptr;
    FilePanel* m_filePanel = nullptr;   // M3-5: 우측 "파일" 탭

    // panel references for toggle
    QWidget* m_leftPanel = nullptr;
    QWidget* m_rightPanel = nullptr;
    QWidget* m_leftToggle = nullptr;
    QWidget* m_rightToggle = nullptr;
    bool m_leftVisible = true;
    bool m_rightVisible = true;

    // settings tab widgets
    QButtonGroup* m_colBtnGroup = nullptr;
    QTabWidget* m_rightTabs = nullptr;
    QTabWidget* m_videoTabs = nullptr;   // 중앙 영상 탭(0="전체" 그리드, 1+=전체화면 단일채널)

    // status bar labels
    QLabel* m_statusChannels = nullptr;
    QLabel* m_statusCpu = nullptr;
    QLabel* m_statusMem = nullptr;

    std::vector<nv::domain::ChannelConfig> m_channels;  // UI cache

    // A1: 채널별 Streaming 여부 추적 (상태바 경보용)
    QMap<QString, bool> m_streaming;
    // 상태바/채널정보 집계용 채널별 최신 지표 (onSnapshot에서 갱신)
    QMap<QString, double> m_pps;          // 채널별 packets/sec(≈fps)
    QMap<QString, double> m_bitrateKbps;  // 채널별 비트레이트
    QMap<QString, qlonglong> m_dropped;   // 채널별 누적 드롭
    QMap<QString, qlonglong> m_decoded;   // 채널별 누적 디코드
    QMap<QString, qlonglong> m_displayed; // 채널별 누적 표시
    QMap<QString, qlonglong> m_readBytes; // 채널별 누적 수신 바이트
    // 채널별 녹화 상태 — 전체화면 탭 녹화 표시 prime용(탭 열 때 현재 상태 반영).
    QMap<QString, nv::domain::RecordingState> m_recStates;
    // 전체화면 탭 라벨의 녹화 ● 표시 갱신(없으면 무동작).
    void updateFullscreenTabRecording(const QString& channelId,
                                      nv::domain::RecordingState state);

    void updateStatusBar();

    // Fix 5: ResourceMonitor 소유권 명시 (unique_ptr — 소멸 자동화)
    std::unique_ptr<nv::infra::ResourceMonitor> m_resourceMonitor;
};

} // namespace nv::ui
