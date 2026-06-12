#pragma once
#include <string>
#include <vector>
#include "src/app/ports/ISnapshotSink.h"

namespace nv::test {

// snapshot 호출을 기록하는 테스트 페이크.
class FakeSnapshotSink final : public nv::app::ISnapshotSink {
public:
    bool snapshot(const std::string& channelId,
                  const std::string& outputPath) override {
        calls.push_back({channelId, outputPath});
        ++callCount;
        return returnValue;
    }

    struct SnapshotCall { std::string channelId; std::string outputPath; };

    std::vector<SnapshotCall> calls;
    int callCount    = 0;
    bool returnValue = true;  // 테스트에서 실패 시나리오 시뮬레이션 가능
};

} // namespace nv::test
