#include "PngSnapshotWriter.h"

#include <QImage>
#include <QString>
#include <cstdio>

namespace nv::infra {

bool PngSnapshotWriter::write(const std::string& path, int w, int h, const uint8_t* rgbaTight) {
    if (rgbaTight == nullptr || w <= 0 || h <= 0) {
        std::fprintf(stderr, "[PngSnapshotWriter] 유효하지 않은 입력 (w=%d h=%d rgba=%p)\n",
                     w, h, static_cast<const void*>(rgbaTight));
        return false;
    }

    // tight RGBA(stride=w*4)를 QImage로 감싼다 — 외부 버퍼를 참조만 하므로 copy()로
    // 깊은 복사본을 만들어 호출자 버퍼 수명과 분리한 뒤 PNG로 저장한다.
    const QImage view(rgbaTight, w, h, w * 4, QImage::Format_RGBA8888);
    const QImage owned = view.copy();   // 깊은 복사 (소유 버퍼)

    if (!owned.save(QString::fromStdString(path), "PNG")) {
        std::fprintf(stderr, "[PngSnapshotWriter] PNG 저장 실패: %s\n", path.c_str());
        return false;
    }
    return true;
}

} // namespace nv::infra
