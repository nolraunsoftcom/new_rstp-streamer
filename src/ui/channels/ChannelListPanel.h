#pragma once
#include <QWidget>
#include <functional>
#include <vector>
#include "src/domain/channel/ChannelConfig.h"

class QListWidget;
class QPushButton;

namespace nv::ui {

// 좌측 채널 목록 패널. 레거시 viewer setupSidebar 대응.
// 채널 추가/삭제 버튼, 우클릭 메뉴(수정/삭제/재시도).
class ChannelListPanel : public QWidget {
    Q_OBJECT
public:
    struct Callbacks {
        std::function<void()> addRequested;
        std::function<void(std::string id)> editRequested;
        std::function<void(std::string id)> removeRequested;
        std::function<void(std::string id)> retryRequested;
    };

    explicit ChannelListPanel(Callbacks cb, QWidget* parent = nullptr);

    // control → UI (queued): 채널 목록 전체 갱신
    void updateChannels(const std::vector<nv::domain::ChannelConfig>& configs);
    // control → UI (queued): 해당 채널 상태 텍스트 갱신
    void updateStatus(const QString& channelId, const QString& state);

private:
    Callbacks m_cb;
    QListWidget* m_list = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_removeBtn = nullptr;
    std::vector<nv::domain::ChannelConfig> m_configs;
};

} // namespace nv::ui
