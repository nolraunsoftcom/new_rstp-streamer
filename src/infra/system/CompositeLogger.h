#pragma once
#include <functional>
#include <QString>
#include "src/infra/system/StderrLogger.h"

namespace nv::infra {

// ILogger 구현. StderrLogger에 위임 + 선택적 Qt 콜백(있으면 호출).
// control/어댑터 스레드에서 호출되므로 콜백 내부에서 QMetaObject::invokeMethod(Queued) 사용 필요.
class CompositeLogger final : public nv::app::ILogger {
public:
    using Callback = std::function<void(QString)>;

    void setCallback(Callback cb) { m_callback = std::move(cb); }

    void log(nv::app::LogLevel level, std::string_view channelId, std::string_view component,
             std::string_view message,
             nv::domain::DiagnosisReason reason = nv::domain::DiagnosisReason::None) override {
        m_stderr.log(level, channelId, component, message, reason);
        if (m_callback) {
            // 로그 탭 표시용: "[채널][컴포넌트] 메시지" 형식
            QString text;
            if (!channelId.empty())
                text += QStringLiteral("[%1]").arg(QString::fromUtf8(channelId.data(),
                                                                     static_cast<int>(channelId.size())));
            if (!component.empty())
                text += QStringLiteral("[%1]").arg(QString::fromUtf8(component.data(),
                                                                     static_cast<int>(component.size())));
            if (!text.isEmpty()) text += QLatin1Char(' ');
            text += QString::fromUtf8(message.data(), static_cast<int>(message.size()));
            m_callback(text);
        }
    }

private:
    StderrLogger m_stderr;
    Callback m_callback;
};

} // namespace nv::infra
