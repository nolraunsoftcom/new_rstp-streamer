#pragma once
#include <chrono>
#include <string_view>

// 순수 도메인 — Qt/FFmpeg include 없음.
namespace nv::domain {

// 녹화 표시 상태. (Stopping은 사용하지 않음 — stopRecording이 비동기지만 완료 신호가 없어
// 컨트롤러는 Idle로 즉시 전이하고 "녹화 저장됨" 토스트가 완료를 알린다.)
enum class RecordingState { Idle, Starting, Recording };

constexpr std::string_view toString(RecordingState s) {
    switch (s) {
        case RecordingState::Idle:      return "Idle";
        case RecordingState::Starting:  return "Starting";
        case RecordingState::Recording: return "Recording";
    }
    return "Unknown";
}

struct SegmentPolicy {
    std::chrono::seconds maxDuration{600};  // 기본 10분
    bool splitOnReconnect = true;
};

} // namespace nv::domain
