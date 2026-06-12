#pragma once
#include <memory>
#include <string>
#include "IStreamSource.h"

namespace nv::app {

// 채널별 스트림 소스 생성. 구현(infra)은 프레임 슬롯 등록·마샬링 래핑까지 책임진다.
// 반환된 소스는 호출자(ChannelManager)가 수명을 소유한다.
class IChannelRuntimeFactory {
public:
    virtual ~IChannelRuntimeFactory() = default;
    virtual std::unique_ptr<IStreamSource> createSource(const std::string& channelId) = 0;
    virtual void destroySource(const std::string& channelId) = 0;  // 슬롯 등 부속 정리
};

} // namespace nv::app
