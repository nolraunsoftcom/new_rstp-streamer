#pragma once
#include <functional>

namespace nv::app {

// 직렬 실행 보장: post된 작업은 단일 스레드에서 순서대로 실행된다.
// ChannelController는 이 스레드(control 스레드)에서만 만져야 한다 (설계 D6).
class IExecutor {
public:
    virtual ~IExecutor() = default;
    virtual void post(std::function<void()> fn) = 0;
};

} // namespace nv::app
