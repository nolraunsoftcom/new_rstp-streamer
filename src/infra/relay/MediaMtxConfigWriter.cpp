#include "MediaMtxConfigWriter.h"
#include <QDir>
#include <QSaveFile>
#include <QString>
#include <cstdio>

namespace nv::infra {

bool MediaMtxConfigWriter::write(const std::string& path, const std::string& yml) {
    const QString qpath = QString::fromStdString(path);

    // 상위 디렉토리가 없으면 생성
    const QDir parentDir = QFileInfo(qpath).dir();
    if (!parentDir.exists()) {
        if (!QDir().mkpath(parentDir.absolutePath())) {
            std::fprintf(stderr, "[MediaMtxConfigWriter] 디렉토리 생성 실패: %s\n",
                         parentDir.absolutePath().toStdString().c_str());
            return false;
        }
    }

    QSaveFile f(qpath);   // 원자적 쓰기
    if (!f.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "[MediaMtxConfigWriter] 파일 열기 실패: %s\n",
                     path.c_str());
        return false;
    }
    f.write(QByteArray::fromStdString(yml));
    if (!f.commit()) {
        std::fprintf(stderr, "[MediaMtxConfigWriter] commit 실패: %s\n",
                     path.c_str());
        return false;
    }
    return true;
}

std::function<bool(const std::string&, const std::string&)> MediaMtxConfigWriter::asCallback() {
    return [this](const std::string& p, const std::string& y) { return write(p, y); };
}

} // namespace nv::infra
