#pragma once
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QString>
#include <string>

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
    // 파일명에 안전하지 않은 문자를 '_'로 치환한다.
    static QString sanitize(const std::string& name) {
        QString s = QString::fromStdString(name);
        if (s.isEmpty()) s = QStringLiteral("channel");
        for (QChar& c : s) {
            const ushort u = c.unicode();
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|' || u < 0x20) {
                c = QLatin1Char('_');
            }
        }
        return s;
    }

    static std::string makePath(const std::string& channelName, const char* ext) {
        const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
        const QString file = sanitize(channelName) + QLatin1Char('_') + ts +
                             QLatin1Char('.') + QLatin1String(ext);
        QDir dir(QString::fromStdString(baseDir()));
        return dir.filePath(file).toStdString();
    }
};

} // namespace nv::infra
