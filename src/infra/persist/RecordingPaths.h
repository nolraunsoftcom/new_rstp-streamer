#pragma once
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QString>
#include <atomic>
#include <string>
#include "src/domain/recording/FileName.h"   // 공용 sanitizeFileName(app·infra 동일 규칙)

namespace nv::infra {

// 녹화/스냅샷 저장 경로 규칙 (설계: QStandardPaths(Movies)/앱폴더, 채널명+타임스탬프).
//
// 디렉토리: QStandardPaths::MoviesLocation/new_viewer/ — Movies가 비면 AppData로 폴백.
// 파일명: <channelName>_<yyyyMMdd_HHmmss>.<ext>  (ext = "mkv" 녹화, "png" 스냅샷)
//
// 헤더 전용(Qt Core만 사용) — JsonChannelRepository와 동일하게 infra에서 Qt Core 허용.
// 채널명에 경로 구분자/제어문자가 있으면 '_'로 치환해 안전한 파일명을 만든다.
class RecordingPaths {
public:
    // 산출물 저장 베이스 디렉토리(없으면 생성). 절대 경로 문자열.
    static std::string baseDir() {
        QString base = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        if (base.isEmpty()) {
            base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        }
        QDir dir(base);
        const QString sub = QStringLiteral("new_viewer");
        dir.mkpath(sub);                        // 멱등 — 이미 있으면 무동작
        return dir.filePath(sub).toStdString();
    }

    // 녹화 파일 전체 경로. 디렉토리 보장 후 <base>/<channel>_<ts>.mkv 반환.
    static std::string recordingPath(const std::string& channelName) {
        return makePath(channelName, "mkv");
    }

    // 스냅샷 파일 전체 경로. <base>/<channel>_<ts>.png 반환.
    static std::string snapshotPath(const std::string& channelName) {
        return makePath(channelName, "png");
    }

private:
    // 파일명 살균 — 공용 도메인 규칙(nv::domain::sanitizeFileName) 사용.
    // 금지 문자는 모두 ASCII라 UTF-8 바이트 단위 치환이 안전(멀티바이트 보존).
    static QString sanitize(const std::string& name) {
        return QString::fromStdString(nv::domain::sanitizeFileName(name));
    }

    static std::string makePath(const std::string& channelName, const char* ext) {
        // D2 충돌 방지: 초 단위 타임스탬프만으로는 같은 초의 stop→start(롤오버)나 연속
        // 스냅샷이 동일 경로가 되어 직전 파일을 덮어쓴다. 밀리초(zzz) + 프로세스 단조
        // 카운터를 붙여 같은 초·같은 밀리초라도 항상 다른 경로를 보장한다.
        static std::atomic<unsigned> s_seq{0};
        const QString ts =
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
        const unsigned seq = s_seq.fetch_add(1, std::memory_order_relaxed) % 1000;
        const QString file = sanitize(channelName) + QLatin1Char('_') + ts +
                             QStringLiteral("_%1").arg(seq, 3, 10, QLatin1Char('0')) +
                             QLatin1Char('.') + QLatin1String(ext);
        QDir dir(QString::fromStdString(baseDir()));
        return dir.filePath(file).toStdString();
    }
};

} // namespace nv::infra
