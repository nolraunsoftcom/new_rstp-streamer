#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "src/domain/relay/RelayConfig.h"
#include "src/domain/health/DiagnosisReason.h"
#include "src/app/ports/IRelayServiceManager.h"
#include "src/app/ports/IRelayControlApi.h"
#include "src/app/ports/ILogger.h"

namespace nv::app {

struct RelayChannelHealth {
    bool up = false;
    nv::domain::DiagnosisReason reason = nv::domain::DiagnosisReason::None;
};

// yml 기록 콜백: (configPath, ymlContent) -> 성공여부. infra writer를 주입.
using RelayConfigWriter = std::function<bool(const std::string& path, const std::string& yml)>;

class RelaySupervisor {
public:
    RelaySupervisor(IRelayServiceManager& svc, IRelayControlApi& api,
                    ILogger& log, RelayConfigWriter writer);

    // ① 생성 ② 검증 ④ 기동요청. 검증 실패/기록 실패 시 false(서비스 기동 안 함). 멱등(startup-only).
    bool ensureUp(const std::vector<nv::domain::RelayPath>& channels,
                  const std::string& configPath);

    // 셀프힐: 서비스를 완전 정리(stop)한 뒤 ensureUp으로 재생성·재기동한다. wedge된(살아있으나
    // 응답 없는) mediamtx를 ensureUp 멱등으로는 못 살리므로, 헬스 지속 실패 시 강제 재기동용.
    bool restart(const std::vector<nv::domain::RelayPath>& channels,
                 const std::string& configPath);

    // ③ relayChannels(useRelay=true인 것만) 각각의 장비→relay leg 상태.
    // API 무응답이면 전부 RelayDown.
    std::map<std::string, RelayChannelHealth> pollHealth(
        const std::vector<nv::domain::RelayPath>& relayChannels);

private:
    IRelayServiceManager& m_svc;
    IRelayControlApi&     m_api;
    ILogger&              m_log;
    RelayConfigWriter     m_writer;
};

} // namespace nv::app
