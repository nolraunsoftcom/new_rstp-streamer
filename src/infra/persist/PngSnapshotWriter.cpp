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

    // 원자적 쓰기: temp 경로에 먼저 저장 후 최종 경로로 rename.
    // FilePanel의 QFileSystemWatcher가 쓰는 중인 파일을 디코드해 나던
    // libpng Read Error를 방지한다(완성된 파일만 노출).
    const std::string tmp = path + ".tmp";
    if (!view.save(QString::fromStdString(tmp), "PNG")) {
        std::fprintf(stderr, "[PngSnapshotWriter] PNG 저장 실패: %s\n", tmp.c_str());
        return false;
    }
    // 원자적 교체: watcher가 쓰는 중 파일을 디코드하지 않도록 temp→최종 rename(같은 FS, 원자적).
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::fprintf(stderr, "[PngSnapshotWriter] rename 실패: %s\n", path.c_str());
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace nv::infra
