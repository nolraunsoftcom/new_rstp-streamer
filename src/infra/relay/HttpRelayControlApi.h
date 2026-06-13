#pragma once
#include "src/app/ports/IRelayControlApi.h"
#include <chrono>
#include <string>

namespace nv::infra {

// mediamtx Control API(/v3/paths/list)를 HTTP GET으로 조회해 RelayPathHealth를 반환한다.
// QNetworkAccessManager + QEventLoop 동기 래퍼 — Qt event loop(QCoreApplication)가 필요하다.
// 생산 환경에서는 control 스레드(Qt event loop 보유)에서 호출할 것.
// 타임아웃 또는 HTTP 오류 시 빈 벡터 반환 → 호출자가 RelayDown으로 해석한다.
class HttpRelayControlApi : public nv::app::IRelayControlApi {
public:
    explicit HttpRelayControlApi(
        std::string apiBase = "http://127.0.0.1:9997",
        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

    std::vector<nv::app::RelayPathHealth> pathsHealth() override;

private:
    std::string m_apiBase;
    std::chrono::milliseconds m_timeout;
};

} // namespace nv::infra
