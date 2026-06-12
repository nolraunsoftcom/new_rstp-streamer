#pragma once
#include <string>

// 순수 app 포트 — Qt/FFmpeg include 없음.
// app 레이어가 infra에 "이 채널 스냅샷 저장"을 명령하는 인터페이스.
// 구현(infra)은 슬롯 RGBA → PNG 저장.
//
// 스레드 계약(D3): 모든 메서드는 control 스레드(ControlExecutor)에서만 호출된다.
// 구현체(ChannelSourceFactory)는 이 불변량에 의존해 채널 슬롯/Bundle 조회를 안전하게 한다.
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
