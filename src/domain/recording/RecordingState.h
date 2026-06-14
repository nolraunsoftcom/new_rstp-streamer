#pragma once
#include <chrono>
#include <string_view>

// 순수 도메인 — Qt/FFmpeg include 없음.
namespace nv::domain {

enum class RecordingState { Idle, Starting, Recording, Stopping };

constexpr std::string_view toString(RecordingState s) {
    switch (s) {
        case RecordingState::Idle:      return "Idle";
        case RecordingState::Starting:  return "Starting";
        case RecordingState::Recording: return "Recording";
        case RecordingState::Stopping:  return "Stopping";
    }
    return "Unknown";
}

struct SegmentPolicy {
    std::chrono::seconds maxDuration{600};  // 기본 10분
    bool splitOnReconnect = true;
};

} // namespace nv::domain
