#pragma once
#include "src/domain/connection/ConnectionStateMachine.h"
#include "src/domain/health/StreamHealth.h"

namespace nv::app {

// control 스레드에서 만들어져 UI로 복사 전달되는 채널 상태 스냅샷.
struct ChannelSnapshot {
    nv::domain::ConnState state = nv::domain::ConnState::Idle;
    int attempts = 0;
    nv::domain::DiagnosisReason reason = nv::domain::DiagnosisReason::None;
    nv::domain::StreamHealth health;

    bool operator==(const ChannelSnapshot&) const = default;
};

} // namespace nv::app
