#pragma once
#include <string>
#include <vector>
#include "src/app/ports/ILogger.h"

namespace nv::test {

class FakeLogger final : public nv::app::ILogger {
public:
    struct Entry {
        nv::app::LogLevel level;
        std::string channelId;
        std::string component;
        std::string message;
        nv::domain::DiagnosisReason reason;
    };

    void log(nv::app::LogLevel level, std::string_view channelId, std::string_view component,
             std::string_view message, nv::domain::DiagnosisReason reason) override {
        entries.push_back({level, std::string(channelId), std::string(component),
                           std::string(message), reason});
    }

    std::vector<Entry> entries;
};

} // namespace nv::test
