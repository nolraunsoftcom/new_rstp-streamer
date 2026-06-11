#pragma once
#include <QObject>
#include "src/app/ChannelSnapshot.h"

namespace nv::ui {

// control 스레드의 스냅샷을 Qt queued 신호로 UI 스레드에 전달.
class ControlBridge : public QObject {
    Q_OBJECT
public:
    // control 스레드에서 호출된다. (signal emit은 스레드 안전, 연결은 자동 queued)
    void publish(const nv::app::ChannelSnapshot& s) {
        QList<int> stages;
        for (auto st : nv::domain::kAllHealthStages)
            stages.push_back(static_cast<int>(s.health.stageState(st)));
        emit snapshotChanged(QString::fromUtf8(toString(s.state).data(),
                                               static_cast<int>(toString(s.state).size())),
                             s.attempts,
                             QString::fromUtf8(toString(s.reason).data(),
                                               static_cast<int>(toString(s.reason).size())),
                             stages);
    }

signals:
    void snapshotChanged(QString state, int attempts, QString reason, QList<int> stages);
};

} // namespace nv::ui
