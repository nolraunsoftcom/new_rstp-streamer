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
        // DnD 재배열: 새 표시 순서의 id 목록(위→아래). 비어있으면 DnD 무동작.
        std::function<void(std::vector<std::string> order)> reorderRequested;
        // 더블클릭/우클릭 → 전체화면 탭 열기
        std::function<void(std::string id)> fullscreenRequested;
    };

    explicit ChannelListPanel(Callbacks cb, QWidget* parent = nullptr);

    // control → UI (queued): 채널 목록 전체 갱신
    void updateChannels(const std::vector<nv::domain::ChannelConfig>& configs);
    // control → UI (queued): 해당 채널 상태 + 패킷 정보 갱신.
    // pps: 패킷/초, msSince: 마지막 패킷 경과(ms, <0=이력없음).
    void updateStatus(const QString& channelId, const QString& state, const QString& reason,
                      double pps, qlonglong msSince);

private:
    Callbacks m_cb;
    QListWidget* m_list = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_removeBtn = nullptr;
    std::vector<nv::domain::ChannelConfig> m_configs;
};

} // namespace nv::ui
