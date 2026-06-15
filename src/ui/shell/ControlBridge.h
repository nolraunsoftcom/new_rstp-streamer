#pragma once
#include <QObject>
#include "src/app/ChannelSnapshot.h"

namespace nv::ui {

class ControlBridge : public QObject {
    Q_OBJECT
public:
    // control 스레드에서 호출
    void publish(const QString& channelId, const nv::app::ChannelSnapshot& s) {
        QList<int> stages;
        for (auto st : nv::domain::kAllHealthStages)
            stages.push_back(static_cast<int>(s.health.stageState(st)));
        emit snapshotChanged(channelId,
                             QString::fromUtf8(toString(s.state).data(),
                                               static_cast<int>(toString(s.state).size())),
                             s.attempts, stages, s.packetsPerSec,
                             static_cast<qlonglong>(s.msSinceLastPacket),
                             QString::fromUtf8(toString(s.reason).data(),
                                               static_cast<int>(toString(s.reason).size())),
                             s.bitrateKbps,
                             static_cast<qlonglong>(s.droppedFrames),
                             static_cast<qlonglong>(s.decodedFrames),
                             static_cast<qlonglong>(s.displayedFrames),
                             static_cast<qlonglong>(s.readBytesTotal));
    }

signals:
    void snapshotChanged(QString channelId, QString state, int attempts, QList<int> stages,
                         double pps, qlonglong msSinceLastPacket, QString reason,
                         double bitrateKbps, qlonglong droppedFrames,
                         qlonglong decodedFrames, qlonglong displayedFrames,
                         qlonglong readBytesTotal);
};

} // namespace nv::ui
