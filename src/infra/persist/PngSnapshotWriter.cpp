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

    // tight RGBA(stride=w*4)를 QImage로 감싼다.
    // 호출자(SnapTask::run)가 rgba 벡터를 소유하며 save() 완료까지 수명을 보장하므로
    // H3: copy()를 제거해 이중 복사를 방지한다(latest()→frame.rgba 깊은 복사 1회로 충분).
    // QImage::save()는 동기적으로 완료되고, view는 save() 반환 전까지 유효하다.
    const QImage view(rgbaTight, w, h, w * 4, QImage::Format_RGBA8888);

    if (!view.save(QString::fromStdString(path), "PNG")) {
        std::fprintf(stderr, "[PngSnapshotWriter] PNG 저장 실패: %s\n", path.c_str());
        return false;
    }
    return true;
}

} // namespace nv::infra
