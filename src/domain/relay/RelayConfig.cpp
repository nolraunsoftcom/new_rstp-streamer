#include "src/domain/relay/RelayConfig.h"

namespace nv::domain {

std::string RelayConfig::generate(const std::vector<RelayPath>& channels)
{
    std::string yml;
    yml += "logLevel: warn\n";
    yml += "logDestinations: [stdout]\n";
    yml += "api: yes\n";
    yml += "apiAddress: 127.0.0.1:9997\n";
    yml += "rtsp: yes\n";
    yml += "rtspTransports: [tcp]\n";
    yml += "rtspAddress: 127.0.0.1:8554\n";
    yml += "rtmp: no\n";
    yml += "hls: no\n";
    yml += "webrtc: no\n";
    yml += "srt: no\n";
    yml += "pathDefaults:\n";
    yml += "  sourceOnDemand: no\n";
    yml += "paths:\n";
    for (const auto& ch : channels) {
        if (!ch.useRelay) continue;
        yml += "  " + ch.id + ":\n";
        yml += "    source: " + ch.deviceUrl + "\n";
        yml += "    sourceOnDemand: no\n";
        yml += "    rtspTransport: tcp\n";
    }
    return yml;
}

RelayValidateResult RelayConfig::validate(const std::string& yml)
{
    // Rule 1: must contain sourceOnDemand: no
    if (yml.find("sourceOnDemand: no") == std::string::npos) {
        return {false, "sourceOnDemand: no not found"};
    }
    // Rule 2: must NOT contain sourceOnDemand: yes
    if (yml.find("sourceOnDemand: yes") != std::string::npos) {
        return {false, "sourceOnDemand: yes present — shield neutralised"};
    }
    // Rule 3: must contain rtspTransports: [tcp]
    if (yml.find("rtspTransports: [tcp]") == std::string::npos) {
        return {false, "rtspTransports: [tcp] not found — tcp transport missing"};
    }
    return {true, {}};
}

} // namespace nv::domain
