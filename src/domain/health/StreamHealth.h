#pragma once
#include <array>
#include <cstddef>
#include "DiagnosisReason.h"

namespace nv::domain {

// 진단 6단계 (설계 §2 신규 기능). 순서가 곧 파이프라인 순서다.
enum class HealthStage : std::size_t {
    DeviceReach = 0,  // 장비 도달
    RelayIntake = 1,  // 장비→Relay 수신 (직결 모드에서는 NotApplicable)
    RtspSession = 2,  // RTSP 세션 열림
    PacketFlow  = 3,  // 패킷 수신 중
    Decoding    = 4,  // 디코딩 성공
    Presenting  = 5,  // 프레임 표시
};

inline constexpr std::array kAllHealthStages = {
    HealthStage::DeviceReach, HealthStage::RelayIntake, HealthStage::RtspSession,
    HealthStage::PacketFlow,  HealthStage::Decoding,    HealthStage::Presenting,
};

enum class StageState { Unknown, Ok, Failed, NotApplicable };

class StreamHealth {
public:
    // stage까지(이전 단계 포함) 도달을 기록. NotApplicable 단계는 건너뛴다.
    void markReached(HealthStage stage) {
        const auto idx = static_cast<std::size_t>(stage);
        for (std::size_t i = 0; i <= idx; ++i) {
            if (m_states[i] != StageState::NotApplicable) m_states[i] = StageState::Ok;
        }
    }

    void markFailed(HealthStage stage, DiagnosisReason reason) {
        m_states[static_cast<std::size_t>(stage)] = StageState::Failed;
        m_reason = reason;
    }

    void markNotApplicable(HealthStage stage) {
        m_states[static_cast<std::size_t>(stage)] = StageState::NotApplicable;
    }

    void reset() {
        for (auto& s : m_states) {
            if (s != StageState::NotApplicable) s = StageState::Unknown;
        }
        m_reason = DiagnosisReason::None;
        // NotApplicable은 모드 속성이므로 reset에도 유지된다.
    }

    StageState stageState(HealthStage stage) const {
        return m_states[static_cast<std::size_t>(stage)];
    }

    DiagnosisReason failedReason() const { return m_reason; }

private:
    std::array<StageState, kAllHealthStages.size()> m_states{};  // 모두 Unknown(0)
    DiagnosisReason m_reason = DiagnosisReason::None;
};

} // namespace nv::domain
