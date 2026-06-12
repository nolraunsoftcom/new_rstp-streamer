#include "SnapshotService.h"

// 순수 app 레이어 — Qt/FFmpeg include 없음.
namespace nv::app {

SnapshotService::SnapshotService(ISnapshotSink& sink, ILogger& logger)
    : m_sink(sink), m_logger(logger) {}

bool SnapshotService::capture(const std::string& channelId,
                              const std::string& channelName,
                              const std::string& outputPath) {
    (void)channelName;  // 경로 생성을 infra에 위임하므로 현재 미사용.
                        // infra 구현이 channelName 기반 경로를 쓰고 싶으면
                        // ISnapshotSink 인터페이스를 확장하거나 outputPath에 담아 전달.
    const bool ok = m_sink.snapshot(channelId, outputPath);
    if (!ok) {
        m_logger.log(LogLevel::Warn, channelId, "SnapshotService",
                     "스냅샷 저장 실패: " + outputPath);
    }
    return ok;
}

} // namespace nv::app
