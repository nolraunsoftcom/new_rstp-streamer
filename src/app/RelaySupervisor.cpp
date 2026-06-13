#include "RelaySupervisor.h"

// 순수 app 레이어 — Qt/FFmpeg include 없음.
namespace nv::app {

RelaySupervisor::RelaySupervisor(IRelayServiceManager& svc, IRelayControlApi& api,
                                 ILogger& log, RelayConfigWriter writer)
    : m_svc(svc), m_api(api), m_log(log), m_writer(std::move(writer)) {}

bool RelaySupervisor::ensureUp(const std::vector<nv::domain::RelayPath>& channels,
                                const std::string& configPath)
{
    // ① 생성
    const std::string yml = nv::domain::RelayConfig::generate(channels);

    // ② 검증
    const auto v = nv::domain::RelayConfig::validate(yml);
    if (!v.ok) {
        m_log.log(LogLevel::Warn, "", "RelaySupervisor",
                  "mediamtx.yml 검증 실패: " + v.reason);
        return false;
    }

    // 기록
    if (!m_writer(configPath, yml)) {
        m_log.log(LogLevel::Warn, "", "RelaySupervisor",
                  "mediamtx.yml 기록 실패: " + configPath);
        return false;
    }

    // ④ 기동요청
    const bool ok = m_svc.ensureRunning(configPath);
    if (ok) {
        m_log.log(LogLevel::Info, "", "RelaySupervisor",
                  "MediaMTX 서비스 기동 요청 완료: " + configPath);
    }
    return ok;
}

std::map<std::string, RelayChannelHealth> RelaySupervisor::pollHealth(
    const std::vector<nv::domain::RelayPath>& relayChannels)
{
    // ③ Control API 헬스 조회
    const auto health = m_api.pathsHealth();

    // 이름으로 빠른 조회를 위한 맵 구성
    std::map<std::string, const RelayPathHealth*> byName;
    for (const auto& h : health) {
        byName[h.name] = &h;
    }

    std::map<std::string, RelayChannelHealth> result;
    for (const auto& ch : relayChannels) {
        if (!ch.useRelay) continue;  // 직결 채널은 결과에 포함 안 함

        if (health.empty()) {
            // API 무응답 — 릴레이 서비스 자체가 다운
            result[ch.id] = {false, nv::domain::DiagnosisReason::RelayDown};
            continue;
        }

        auto it = byName.find(ch.id);
        if (it == byName.end()) {
            // path 자체가 없음
            result[ch.id] = {false, nv::domain::DiagnosisReason::RelayNoSource};
            continue;
        }

        const RelayPathHealth& ph = *it->second;
        if (ph.hasSource && ph.ready) {
            result[ch.id] = {true, nv::domain::DiagnosisReason::None};
        } else {
            result[ch.id] = {false, nv::domain::DiagnosisReason::RelayNoSource};
        }
    }
    return result;
}

} // namespace nv::app
