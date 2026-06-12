#pragma once
#include <QObject>
#include <QTimer>
#include <map>
#include <string>
#include "src/infra/ffmpeg/ChannelSourceFactory.h"

namespace nv::infra {

// 소크 통계 로거 — 60초마다 RSS·fps·활성 채널 수를 CSV(4컬럼)로 기록.
// 파일 회전: 10MB 초과 시 기존 파일을 .1로 백업 후 새 파일 시작.
// 경로: QStandardPaths::AppConfigLocation 하위 logs/ 디렉토리 (절대경로).
class SoakLogger : public QObject {
    Q_OBJECT
public:
    // csvPath: 절대 경로 (예: /Users/.../logs/soak.csv). QDir::mkpath는 호출자가.
    explicit SoakLogger(ChannelSourceFactory& factory, QString csvPath,
                        QObject* parent = nullptr);

    void start(int intervalMs = 60'000);   // intervalMs는 멤버에 저장 → fps 계산에 사용
    void stop();

private slots:
    void onTimer();

private:
    void rotatIfNeeded();

    ChannelSourceFactory& m_factory;
    QString m_csvPath;
    QTimer m_timer;
    std::map<std::string, uint64_t> m_lastSeqs;

    static constexpr qint64 kMaxFileSizeBytes = 10LL * 1024 * 1024; // 10 MB
    int m_intervalMs = 60'000;   // R5: start()의 인자 보존 → fps = frames / (intervalMs/1000)
};

} // namespace nv::infra
