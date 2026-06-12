#pragma once
#include <QWidget>
#include <string>
#include "src/app/ports/IFrameSurface.h"
#include "src/app/ports/IFrameSurfaceRegistry.h"
#include "src/ui/render/RendererSelect.h"

class QVBoxLayout;

namespace nv::app { class IVideoRenderer; }

namespace nv::ui {

class SwVideoRenderer;
class RhiVideoRenderer;

// IFrameSurfaceRegistry 포트로 최신 서피스를 폴링해 활성 IVideoRenderer에 present.
// 자체 QTimer 없음 — 앱 전체 단일 RepaintClock이 ~30Hz 펄스를 발행한다.
// 기본은 GPU(RhiVideoRenderer); QRhi 초기화 실패(renderFailed) 시 런타임에 SW로 폴백한다.
// 렌더러가 새 프레임을 실제 표시하면 framePainted 발신 (= 도메인의 "표시 확정").
class VideoTileWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoTileWidget(nv::app::IFrameSurfaceRegistry& registry,
                             std::string channelId,
                             QWidget* parent = nullptr);

signals:
    void framePainted();

public slots:
    void pollFrame();

private:
    void installRenderer(RendererKind kind);
    void fallbackToSw();

    nv::app::IFrameSurfaceRegistry& m_registry;
    std::string m_channelId;
    QVBoxLayout* m_layout = nullptr;
    nv::app::IVideoRenderer* m_renderer = nullptr;   // m_layout 소유 위젯(비소유 포인터)
    RendererKind m_kind = RendererKind::Sw;
    uint64_t m_seq = 0;
    bool m_fellBack = false;   // 이미 SW 폴백했으면 재시도 안 함
};

} // namespace nv::ui
