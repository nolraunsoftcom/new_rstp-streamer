#pragma once
#include <QDialog>
#include "src/domain/channel/ChannelConfig.h"
#include "src/domain/recording/RecordingState.h"

namespace nv::ui {

// 채널 정보 다이얼로그. 레거시 ChannelInfoDialog는 libVLC 미디어 통계 트리였으나,
// 우리 FFmpeg 파이프라인엔 해당 통계가 없어 구성 정보(이름/URL/모드/상태/녹화)를 보여준다.
// 정적 스냅샷 — 열 때의 상태를 표시(라이브 갱신 없음).
class ChannelInfoDialog : public QDialog {
    Q_OBJECT
public:
    ChannelInfoDialog(const nv::domain::ChannelConfig& cfg,
                      bool streaming,
                      nv::domain::RecordingState recState,
                      QWidget* parent = nullptr);
};

} // namespace nv::ui
