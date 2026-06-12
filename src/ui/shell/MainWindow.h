#pragma once
#include <QMainWindow>
#include <QVector>
#include <functional>
#include <memory>
#include <vector>
#include "src/domain/channel/ChannelConfig.h"
#include "src/infra/system/ResourceMonitor.h"

class QComboBox;
class QButtonGroup;
class QLabel;
class QTabWidget;

namespace nv::ui {
class GridView;
class ChannelListPanel;
class LogPanel;

// 레거시 패리티 3패널 셸:
//   좌측 ChannelListPanel + 토글 | 중앙 QTabWidget(전체/GridView, 검정 배경) | 토글 + 우측 패널(설정/파일/로그)
// 상태바: 채널 집계 + CPU/메모리 (ResourceMonitor 1초 주기)
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    struct Commands {
        std::function<void(std::string name, std::string url, bool autoConnect)> addChannel;
        std::function<void(std::string id, std::string name, std::string url, bool autoConnect)> updateChannel;
        std::function<void(std::string id)> removeChannel;
        std::function<void(std::string id)> retryChannel;
        std::function<void(std::string id)> framePainted;
        std::function<void(std::string a, std::string b)> swapChannels;
    };

    MainWindow(GridView* grid, ChannelListPanel* channelPanel, LogPanel* logPanel,
               Commands commands);

    int manualColumns() const;  // 0 = Auto

    // public so main.cpp gridCb.editRequested can call it
    void openEditDialog(const std::string& id);

public slots:
    void onChannelList(QVector<QString> ids, QVector<QString> names, QVector<QString> urls,
                       QVector<int> gridIndexes, QVector<bool> autoConnects);
    void onSnapshot(QString channelId, QString state, int attempts, QList<int> stages,
                    double pps, qlonglong msSinceLastPacket, QString reason);

private:
    void rebuildGrid();
    void openAddDialog();
    void setLeftPanelVisible(bool visible);
    void setRightPanelVisible(bool visible);
    void updateToggleButtons();

    Commands m_commands;
    GridView* m_grid = nullptr;
    ChannelListPanel* m_channelPanel = nullptr;
    LogPanel* m_logPanel = nullptr;

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

    // status bar labels
    QLabel* m_statusChannels = nullptr;
    QLabel* m_statusCpu = nullptr;
    QLabel* m_statusMem = nullptr;

    std::vector<nv::domain::ChannelConfig> m_channels;  // UI cache

    // Fix 5: ResourceMonitor 소유권 명시 (unique_ptr — 소멸 자동화)
    std::unique_ptr<nv::infra::ResourceMonitor> m_resourceMonitor;
};

} // namespace nv::ui
