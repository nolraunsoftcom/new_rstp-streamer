#pragma once
#include "IFrameSurface.h"

class QWidget;

namespace nv::app {

// 서피스 1장을 위젯 위에 그리는 렌더러 포트. 구현은 ui 계층(SW: QPainter, GPU: QRhiWidget).
// app/domain은 Qt 위젯 타입을 모르므로 widget()은 불투명 QWidget*로만 노출한다.
// 수명: 렌더러가 자신의 QWidget을 소유하지 않는다 — 호스트(VideoTileWidget)가 부모로 두고 소유.
class IVideoRenderer {
public:
    virtual ~IVideoRenderer() = default;

    // 표시할 서피스를 건넨다. 구현은 다음 페인트/렌더에서 이 서피스를 그린다.
    // GpuTexture 서피스의 gpuHandle 수명 계약은 LatestSurfaceSlot 헤더 주석 참조
    // (소비자가 그린 뒤 releaseConsumed로 반납). 동반 RGBA만 쓰는 구현은 핸들을 무시한다.
    virtual void present(const FrameSurface& surface) = 0;

    // 호스트 레이아웃에 삽입할 실제 위젯. 렌더러별 1개.
    virtual QWidget* widget() = 0;
};

} // namespace nv::app
