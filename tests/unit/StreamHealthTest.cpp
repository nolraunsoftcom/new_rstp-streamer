#include <catch2/catch_test_macros.hpp>
#include "src/domain/health/DiagnosisReason.h"
#include "src/domain/health/StreamHealth.h"

using namespace nv::domain;

TEST_CASE("DiagnosisReason: 모든 코드에 사람이 읽을 라벨이 있다") {
    CHECK(toString(DiagnosisReason::None) == "None");
    CHECK(toString(DiagnosisReason::DeviceUnreachable) == "DeviceUnreachable");
    CHECK(toString(DiagnosisReason::RelayDown) == "RelayDown");
    CHECK(toString(DiagnosisReason::RelayNoSource) == "RelayNoSource");
    CHECK(toString(DiagnosisReason::SessionRefused) == "SessionRefused");
    CHECK(toString(DiagnosisReason::NoPackets) == "NoPackets");
    CHECK(toString(DiagnosisReason::DecodeError) == "DecodeError");
    CHECK(toString(DiagnosisReason::DiskLow) == "DiskLow");
    CHECK(toString(DiagnosisReason::DiskFull) == "DiskFull");
    CHECK(toString(DiagnosisReason::GaveUp) == "GaveUp");
}

TEST_CASE("StreamHealth: 초기 상태는 모든 단계 Unknown") {
    StreamHealth h;
    for (auto stage : kAllHealthStages) {
        CHECK(h.stageState(stage) == StageState::Unknown);
    }
    CHECK(h.failedReason() == DiagnosisReason::None);
}

TEST_CASE("StreamHealth: 단계 도달을 기록하면 그 단계까지 Ok") {
    StreamHealth h;
    h.markReached(HealthStage::PacketFlow);
    CHECK(h.stageState(HealthStage::RtspSession) == StageState::Ok);  // 이전 단계도 Ok
    CHECK(h.stageState(HealthStage::PacketFlow) == StageState::Ok);
    CHECK(h.stageState(HealthStage::Decoding) == StageState::Unknown);
}

TEST_CASE("StreamHealth: 실패를 기록하면 해당 단계 Failed + 원인 보존") {
    StreamHealth h;
    h.markReached(HealthStage::RtspSession);
    h.markFailed(HealthStage::PacketFlow, DiagnosisReason::NoPackets);
    CHECK(h.stageState(HealthStage::PacketFlow) == StageState::Failed);
    CHECK(h.failedReason() == DiagnosisReason::NoPackets);
    CHECK(h.stageState(HealthStage::RtspSession) == StageState::Ok); // 도달한 단계는 유지
}

TEST_CASE("StreamHealth: reset은 전 단계 Unknown으로 (재접속 사이클 시작)") {
    StreamHealth h;
    h.markReached(HealthStage::Presenting);
    h.reset();
    CHECK(h.stageState(HealthStage::Presenting) == StageState::Unknown);
    CHECK(h.failedReason() == DiagnosisReason::None);
}

TEST_CASE("RelayIntake 단계는 직결 모드에서 NotApplicable로 둘 수 있다") {
    StreamHealth h;
    h.markNotApplicable(HealthStage::RelayIntake);
    h.markReached(HealthStage::PacketFlow);
    CHECK(h.stageState(HealthStage::RelayIntake) == StageState::NotApplicable);
}
