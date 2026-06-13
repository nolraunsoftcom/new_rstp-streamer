#pragma once
#include <string>

namespace nv::domain {

struct ChannelConfig {
    std::string id;        // 불변 고유 식별자 ("ch<n>") — 생성 시 부여
    std::string name;
    std::string url;
    int gridIndex = -1;    // 그리드 셀 위치. -1 = 미배치(manager가 부여)
    bool autoConnect = false;  // 앱 시작 시 자동 연결 여부
    bool useRelay = false;     // true: relay 경유 연결(relayUrlFor 사용), false: 직결

    bool operator==(const ChannelConfig&) const = default;
};

// relay 경유 시 앱이 실제 연결할 URL. path 이름 = 채널 id.
inline std::string relayUrlFor(const std::string& channelId) {
    return "rtsp://127.0.0.1:8554/" + channelId;
}

} // namespace nv::domain
