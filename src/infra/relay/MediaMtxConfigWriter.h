#pragma once
#include <functional>
#include <string>

namespace nv::infra {

// mediamtx.yml을 디스크에 원자적으로 기록 (QSaveFile + commit).
// RelaySupervisor에 RelayConfigWriter 콜백으로 주입한다.
class MediaMtxConfigWriter {
public:
    // path에 yml을 원자적으로 기록 (상위 디렉토리 없으면 생성). 성공 시 true.
    bool write(const std::string& path, const std::string& yml);

    // RelaySupervisor에 주입할 콜백 어댑터.
    // 사용: RelaySupervisor sup(svc, api, log, writer.asCallback());
    std::function<bool(const std::string&, const std::string&)> asCallback();
};

} // namespace nv::infra
