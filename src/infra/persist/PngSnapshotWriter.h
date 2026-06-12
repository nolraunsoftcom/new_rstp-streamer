#pragma once
#include <cstdint>
#include <string>

namespace nv::infra {

// 디코딩 원본 RGBA 프레임을 PNG로 저장한다 (설계: 오버레이 없는 순수 프레임 스냅샷).
//
// infra에서 Qt(Gui/Core) 사용은 JsonChannelRepository(Qt Core) 선례를 따른다 —
// QImage는 Qt Gui라 nv_infra가 Qt6::Gui에 링크해야 한다(CMake에서 배선).
//
// 입력은 tight RGBA(stride = w*4, Format_RGBA8888). QImage는 외부 버퍼를 참조만 하므로
// save() 전에 copy()로 깊은 복사본을 만들어 호출자 버퍼 수명과 분리한다.
class PngSnapshotWriter {
public:
    // path에 PNG로 저장한다. 성공 시 true.
    // rgbaTight==nullptr / w<=0 / h<=0 이면 false(순수 GPU 경로로 RGBA가 비었을 때 등).
    static bool write(const std::string& path, int w, int h, const uint8_t* rgbaTight);
};

} // namespace nv::infra
