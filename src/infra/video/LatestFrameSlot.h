#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

namespace nv::infra {

// 디코드 스레드 → UI 스레드 최신 프레임 메일박스.
// 백프레셔 정책: 항상 마지막 프레임만 유지 (표시는 최신만 의미 있음, 설계 D3의 소비자 격리).
// 데이터는 타이트팩 RGBA(stride = width*4)로 저장한다.
class LatestFrameSlot {
public:
    struct Frame {
        int width = 0;
        int height = 0;
        uint64_t seq = 0;
        std::vector<uint8_t> rgba;
    };

    void publish(int width, int height, const uint8_t* rgbaTight) {
        std::lock_guard lk(m_mu);
        m_frame.width = width;
        m_frame.height = height;
        m_frame.rgba.assign(rgbaTight, rgbaTight + static_cast<size_t>(width) * height * 4);
        ++m_frame.seq;
    }

    // lastSeenSeq보다 새 프레임이 있으면 out에 복사하고 true.
    bool latest(Frame& out, uint64_t lastSeenSeq) const {
        std::lock_guard lk(m_mu);
        if (m_frame.seq == 0 || m_frame.seq == lastSeenSeq) return false;
        out = m_frame;
        return true;
    }

private:
    mutable std::mutex m_mu;
    Frame m_frame;
};

} // namespace nv::infra
