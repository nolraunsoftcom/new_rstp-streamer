#pragma once
#include <string_view>

namespace nv::domain {

// 설계 D5: UI 문구·로그·테스트가 공유하는 단일 원인 코드.
enum class DiagnosisReason {
    None,
    DeviceUnreachable,
    RelayDown,
    RelayNoSource,
    SessionRefused,
    NoPackets,
    DecodeError,
    DiskLow,
    DiskFull,
    GaveUp,
};

constexpr std::string_view toString(DiagnosisReason r) {
    switch (r) {
        case DiagnosisReason::None:              return "None";
        case DiagnosisReason::DeviceUnreachable: return "DeviceUnreachable";
        case DiagnosisReason::RelayDown:         return "RelayDown";
        case DiagnosisReason::RelayNoSource:     return "RelayNoSource";
        case DiagnosisReason::SessionRefused:    return "SessionRefused";
        case DiagnosisReason::NoPackets:         return "NoPackets";
        case DiagnosisReason::DecodeError:       return "DecodeError";
        case DiagnosisReason::DiskLow:           return "DiskLow";
        case DiagnosisReason::DiskFull:          return "DiskFull";
        case DiagnosisReason::GaveUp:            return "GaveUp";
    }
    return "Unknown";
}

} // namespace nv::domain
