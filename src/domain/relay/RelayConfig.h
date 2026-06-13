#pragma once
#include <string>
#include <vector>
namespace nv::domain {
struct RelayPath {
    std::string id;         // 채널 id → mediamtx path 이름
    std::string deviceUrl;  // 장비 RTSP URL → path의 source
    bool useRelay = false;  // false면 직결(relay path 생성 안 함)
};
struct RelayValidateResult { bool ok = false; std::string reason; };
class RelayConfig {
public:
    // relay 모드 채널만 path로 포함하는 mediamtx.yml 텍스트 생성.
    static std::string generate(const std::vector<RelayPath>& channels);
    // 하드룰 충족 검증: sourceOnDemand:no 존재 + sourceOnDemand:yes 부재 + rtspTransports:[tcp] 존재.
    static RelayValidateResult validate(const std::string& yml);
};
}
