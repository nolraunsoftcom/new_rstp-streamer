#pragma once
#include <string>

// 순수 app 포트 — Qt/FFmpeg include 없음.
// app 레이어가 infra에 "이 채널 스냅샷 저장"을 명령하는 인터페이스.
// 구현(infra)은 슬롯 RGBA → PNG 저장.
namespace nv::app {

class ISnapshotSink {
public:
    virtual ~ISnapshotSink() = default;

    // channelId 채널의 현재 프레임을 outputPath 경로에 PNG로 저장한다.
    // 저장 성공 시 true, 실패(프레임 없음·쓰기 오류) 시 false.
    virtual bool snapshot(const std::string& channelId,
                          const std::string& outputPath) = 0;
};

} // namespace nv::app
