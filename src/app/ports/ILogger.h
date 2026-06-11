#pragma once
#include <string>
#include <string_view>
#include "src/domain/health/DiagnosisReason.h"

namespace nv::app {

enum class LogLevel { Debug, Info, Warn, Error };

// 설계 §6 관측성: 모든 로그는 채널ID·컴포넌트·원인코드를 구조화 필드로 갖는다.
class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void log(LogLevel level, std::string_view channelId, std::string_view component,
                     std::string_view message,
                     nv::domain::DiagnosisReason reason = nv::domain::DiagnosisReason::None) = 0;
};

} // namespace nv::app
