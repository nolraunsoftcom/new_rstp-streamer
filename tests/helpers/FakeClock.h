#pragma once
#include "src/app/ports/IClock.h"

namespace nv::test {

class FakeClock final : public nv::app::IClock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    std::chrono::steady_clock::time_point now() const override { return m_now; }
    void advance(std::chrono::milliseconds d) { m_now += d; }
    void setTo(TimePoint t) { m_now = t; }

private:
    TimePoint m_now{};  // 임의 epoch에서 시작
};

} // namespace nv::test
