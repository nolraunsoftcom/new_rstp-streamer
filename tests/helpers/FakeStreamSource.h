#pragma once
#include <string>
#include "src/app/ports/IStreamSource.h"

namespace nv::test {

// open/close 호출을 기록하고, 테스트가 리스너에 이벤트를 직접 주입하게 한다.
class FakeStreamSource final : public nv::app::IStreamSource {
public:
    void open(const std::string& url, nv::app::StreamSourceListener& listener) override {
        ++openCount;
        lastUrl = url;
        m_listener = &listener;
    }
    void close() override {
        ++closeCount;
        m_listener = nullptr;
    }

    // 테스트 헬퍼: 어댑터가 이벤트를 올리는 상황을 흉내낸다.
    nv::app::StreamSourceListener* listener() { return m_listener; }
    bool isOpen() const { return m_listener != nullptr; }

    int openCount = 0;
    int closeCount = 0;
    std::string lastUrl;

private:
    nv::app::StreamSourceListener* m_listener = nullptr;
};

} // namespace nv::test
