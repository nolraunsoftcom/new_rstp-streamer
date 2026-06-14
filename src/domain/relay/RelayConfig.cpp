#include "src/domain/relay/RelayConfig.h"
#include <cstring>

namespace nv::domain {

namespace {
// YAML 이중따옴표 스칼라로 안전 인코딩: \와 "를 이스케이프, 제어문자(<0x20)는 제거
// (정상 RTSP URL엔 제어문자가 없음 — 개행 주입으로 새 설정 라인을 만드는 공격을 차단).
static std::string yamlQuote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (u < 0x20) { /* 제어문자 제거 */ }
        else out += c;
    }
    out += "\"";
    return out;
}
} // namespace

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
        yml += "    source: " + yamlQuote(ch.deviceUrl) + "\n";
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
    // 주입 방어: mediamtx의 명령 실행 키는 RelayConfig가 절대 생성하지 않는다 — 있으면 주입.
    // 줄 단위로 검사: 각 줄을 ltrim 후 key: 로 시작하는지 확인 — 따옴표 스칼라 값 내부 오탐 방지.
    {
        static const char* const kDangerousKeys[] = {
            "runOnInit","runOnReady","runOnDemand","runOnConnect",
            "runOnDisconnect","runOnRead","runOnUnread","runOnRecordSegmentCreate"
        };
        std::size_t lineStart = 0;
        while (lineStart < yml.size()) {
            const std::size_t lineEnd = yml.find('\n', lineStart);
            const std::size_t end = (lineEnd == std::string::npos) ? yml.size() : lineEnd;
            // ltrim: skip leading spaces/tabs
            std::size_t keyStart = lineStart;
            while (keyStart < end && (yml[keyStart] == ' ' || yml[keyStart] == '\t')) ++keyStart;
            for (const char* key : kDangerousKeys) {
                const std::size_t klen = std::strlen(key);
                if (end - keyStart >= klen + 1 &&
                    yml.compare(keyStart, klen, key) == 0 &&
                    yml[keyStart + klen] == ':') {
                    return {false, std::string("dangerous exec key present: ") + key};
                }
            }
            if (lineEnd == std::string::npos) break;
            lineStart = lineEnd + 1;
        }
    }
    return {true, {}};
}

} // namespace nv::domain
