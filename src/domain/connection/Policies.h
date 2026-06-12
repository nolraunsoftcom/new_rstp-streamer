#pragma once
#include <chrono>

namespace nv::domain {

struct ReconnectPolicy {
    int maxAttempts = 30;                              // 초과 시 Failed (저빈도 모드 전환)
    std::chrono::milliseconds retryDelay{5000};        // 고속 재시도 간격
    std::chrono::milliseconds slowRetryDelay{60000};   // Failed에서의 저빈도 재시도 간격 (D1)
};

struct StallPolicy {
    std::chrono::milliseconds dataConfirmTimeout{5000}; // SessionOpen에서 첫 패킷 대기 한도 (가짜연결 판정)
    std::chrono::milliseconds stallTimeout{10000};      // Streaming에서 패킷 공백 한도
};

} // namespace nv::domain
