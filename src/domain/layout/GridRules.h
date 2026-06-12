#pragma once

namespace nv::domain::grid {

// 설계 §2 Auto 컬럼 규칙. 순수 함수 — UI는 이 값을 그대로 사용한다.
constexpr int autoColumns(int channelCount) {
    if (channelCount <= 0) return 3;
    if (channelCount == 1) return 1;
    if (channelCount <= 4) return 2;
    if (channelCount <= 9) return 3;
    if (channelCount <= 16) return 4;
    return 5;
}

constexpr int rowsFor(int channelCount, int columns) {
    if (channelCount <= 0 || columns <= 0) return 1;
    return (channelCount + columns - 1) / columns;
}

} // namespace nv::domain::grid
