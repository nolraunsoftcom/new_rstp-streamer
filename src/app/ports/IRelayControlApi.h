#pragma once
#include <string>
#include <vector>

namespace nv::app {

struct RelayPathHealth { std::string name; bool ready=false; bool hasSource=false; };

// mediamtx Control API 헬스(장비→Relay 수신 데이터 소스). infra 어댑터가 구현.
class IRelayControlApi {
public:
    virtual ~IRelayControlApi() = default;

    // 각 path의 ready(송출가능)/hasSource(장비 leg 연결됨). API 실패 시 빈 벡터.
    virtual std::vector<RelayPathHealth> pathsHealth() = 0;
};

} // namespace nv::app
