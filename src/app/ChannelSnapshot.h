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
    double bitrateKbps = 0.0;           // 최근 1초 수신 비트레이트(상태바 표시용)

    // 누적 카운터(채널정보 다이얼로그용) — 프레임/패킷마다 증가. 아래 operator==에서 제외해
    // 프레임당 emit 플러드를 막는다(1초 tick의 pps/bitrate 변화 emit에 실려 UI로 전달됨).
    long long droppedFrames = 0;        // 이번 연결 누적 드롭 프레임(디코드/HW전송 실패)
    long long decodedFrames = 0;        // 누적 디코드 프레임
    long long displayedFrames = 0;      // 누적 표시(present) 프레임
    long long readBytesTotal = 0;       // 누적 수신 바이트

    // 누적 카운터는 비교에서 제외 — 매 프레임 변하므로 포함 시 emit이 폭주한다.
    bool operator==(const ChannelSnapshot& o) const {
        return state == o.state && attempts == o.attempts && reason == o.reason
            && health == o.health && packetsPerSec == o.packetsPerSec
            && bitrateKbps == o.bitrateKbps && msSinceLastPacket == o.msSinceLastPacket;
    }
};

} // namespace nv::app
