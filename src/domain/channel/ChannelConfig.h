#pragma once
#include <string>

namespace nv::domain {

struct ChannelConfig {
    std::string id;        // 불변 고유 식별자 ("ch<n>") — 생성 시 부여
    std::string name;
    std::string url;
    int gridIndex = -1;    // 그리드 셀 위치. -1 = 미배치(manager가 부여)

    bool operator==(const ChannelConfig&) const = default;
};

} // namespace nv::domain
