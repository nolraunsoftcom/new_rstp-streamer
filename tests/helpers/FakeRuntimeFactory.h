#pragma once
#include <map>
#include "src/app/ports/IChannelRuntimeFactory.h"
#include "FakeStreamSource.h"

namespace nv::test {

// 채널별 FakeStreamSource를 만들어주고, 테스트가 채널ID로 접근할 수 있게 한다.
class FakeRuntimeFactory final : public nv::app::IChannelRuntimeFactory {
public:
    // 소유권은 ChannelManager로 가지만, 테스트 관찰용 포인터를 registry에 남긴다.
    std::unique_ptr<nv::app::IStreamSource> createSource(const std::string& channelId) override {
        auto src = std::make_unique<FakeStreamSource>();
        registry[channelId] = src.get();
        ++createCount;
        return src;
    }
    void destroySource(const std::string& channelId) override {
        registry.erase(channelId);
        ++destroyCount;
    }

    std::map<std::string, FakeStreamSource*> registry;
    int createCount = 0;
    int destroyCount = 0;
};

} // namespace nv::test
