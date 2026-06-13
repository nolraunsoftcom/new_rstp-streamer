#pragma once
#include <chrono>

namespace nv::domain {

struct ReconnectPolicy {
    int maxAttempts = 30;                              // 초과 시 Failed (저빈도 모드 전환)
    std::chrono::milliseconds retryDelay{5000};        // 고속 재시도 간격
    std::chrono::milliseconds slowRetryDelay{60000};   // Failed에서의 저빈도 재시도 간격 (D1)
    // 지속 스트리밍 리셋: Streaming 상태에서 패킷이 이 시간 이상 끊김 없이 지속되면 재시도
    // 카운터를 리셋한다(표시 framePresented와 무관 — 오프스크린 타일은 프레임을 그리지 않아
    // framePresented가 안 오므로 가시성 독립 복원력을 위해 패킷 흐름으로도 리셋한다). stall
    // 감지도 패킷 기반이라 일관적이다. 윈도우 미만의 짧은 스트리밍(플래핑)은 리셋하지 않는다.
    std::chrono::milliseconds streamingStableReset{3000};
};

struct StallPolicy {
    std::chrono::milliseconds dataConfirmTimeout{5000}; // SessionOpen에서 첫 패킷 대기 한도 (가짜연결 판정)
    std::chrono::milliseconds stallTimeout{10000};      // Streaming에서 패킷 공백 한도
};

} // namespace nv::domain
