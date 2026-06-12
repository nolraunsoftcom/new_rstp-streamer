#pragma once
#include <string>
#include "src/app/ports/ILogger.h"
#include "src/app/ports/ISnapshotSink.h"

// 순수 app 레이어 — Qt/FFmpeg include 없음.
// 채널 스냅샷 요청을 ISnapshotSink(infra)에 위임한다.
// 경로 생성은 sink(infra 구현)가 담당 — app은 channelId/channelName만 전달.
//
// 동기/비동기 선택: infra(PngSnapshotWriter)가 빠르면 sink.snapshot이 인라인으로 완료.
// UI 블로킹이 문제가 되면 infra 측에서 별도 쓰기 스레드를 두면 되며,
// 이 서비스는 그 결정에 무관하다(포트 경계 덕분에).
namespace nv::app {

class SnapshotService {
public:
    SnapshotService(ISnapshotSink& sink, ILogger& logger);

    // channelId 채널의 현재 프레임을 PNG로 저장한다.
    // 경로는 infra(ISnapshotSink 구현)가 결정.
    // 실패 시 경고 로그 + false 반환. 성공 시 true.
    //
    // outputPath: 비어있으면 sink가 기본 경로를 사용한다고 가정.
    //             실제 배선에서는 RecordingPaths(infra)가 main에서 경로를 생성해
    //             여기에 전달하거나, sink 구현이 내부적으로 경로를 결정한다.
    bool capture(const std::string& channelId,
                 const std::string& channelName,
                 const std::string& outputPath);

private:
    ISnapshotSink& m_sink;
    ILogger&       m_logger;
};

} // namespace nv::app
