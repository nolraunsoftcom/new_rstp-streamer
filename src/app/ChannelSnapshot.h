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

    // 실시간 수신 지표 — tick(1초 주기)마다 갱신 (UI 즉시 가시성용; 상태머신 판정과 별개)
    double packetsPerSec = 0.0;
    long long msSinceLastPacket = -1;   // -1 = 이번 사이클 수신 이력 없음

    bool operator==(const ChannelSnapshot&) const = default;
};

} // namespace nv::app
