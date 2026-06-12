#include "SoakLogger.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <chrono>
#include <cstdio>
#include "src/infra/video/LatestSurfaceSlot.h"
#include "src/infra/system/ProcessStats.h"

namespace nv::infra {

SoakLogger::SoakLogger(ChannelSourceFactory& factory, QString csvPath, QObject* parent)
    : QObject(parent), m_factory(factory), m_csvPath(std::move(csvPath))
{
    connect(&m_timer, &QTimer::timeout, this, &SoakLogger::onTimer);
}

void SoakLogger::start(int intervalMs)
{
    m_timer.start(intervalMs);
}

void SoakLogger::stop()
{
    m_timer.stop();
}

void SoakLogger::rotatIfNeeded()
{
    const QFileInfo fi(m_csvPath);
    if (fi.exists() && fi.size() >= kMaxFileSizeBytes) {
        const QString backup = m_csvPath + QStringLiteral(".1");
        QFile::remove(backup);
        QFile::rename(m_csvPath, backup);
    }
}

void SoakLogger::onTimer()
{
    rotatIfNeeded();

    // QFile for safe absolute-path open
    QFile csv(m_csvPath);
    if (!csv.open(QIODevice::Append | QIODevice::Text)) return;

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    double fpsTotal = 0.0;
    int active = 0;
    for (const auto& id : m_factory.slotIds()) {
        if (auto* s = m_factory.slot(id)) {
            LatestSurfaceSlot::Frame f;
            uint64_t seq = m_lastSeqs[id];
            if (s->latest(f, seq)) seq = f.seq;
            const uint64_t prev = m_lastSeqs[id];
            if (seq > prev) {
                // interval은 start()의 intervalMs와 일치하도록 60.0 고정
                fpsTotal += static_cast<double>(seq - prev) / 60.0;
                ++active;
            }
            m_lastSeqs[id] = seq;
        }
    }

    // 4컬럼: epoch_ms, fps_total, rss_mb, active_count
    const QString line = QString::asprintf("%lld,%.1f,%.1f,%d\n",
                                           static_cast<long long>(ms),
                                           fpsTotal,
                                           nv::infra::processRssMb(),
                                           active);
    csv.write(line.toUtf8());
    // csv destructor closes automatically
}

} // namespace nv::infra
