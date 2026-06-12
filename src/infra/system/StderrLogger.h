#pragma once
#include <chrono>
#include <cstdio>
#include "src/app/ports/ILogger.h"

namespace nv::infra {

// 구조화 한 줄 로그: epoch_ms level [channel][component] message (reason)
class StderrLogger final : public nv::app::ILogger {
public:
    void log(nv::app::LogLevel level, std::string_view channelId, std::string_view component,
             std::string_view message, nv::domain::DiagnosisReason reason) override {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        static constexpr const char* kLevels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
        std::fprintf(stderr, "%lld %s [%.*s][%.*s] %.*s (%.*s)\n",
                     static_cast<long long>(ms), kLevels[static_cast<int>(level)],
                     static_cast<int>(channelId.size()), channelId.data(),
                     static_cast<int>(component.size()), component.data(),
                     static_cast<int>(message.size()), message.data(),
                     static_cast<int>(toString(reason).size()), toString(reason).data());
    }
};

} // namespace nv::infra
