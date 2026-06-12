#pragma once
#include <chrono>
#include <functional>
#include <map>
#include <string>
#include "src/app/ports/IClock.h"
#include "src/app/ports/ILogger.h"
#include "src/app/ports/IRecordingSink.h"
#include "src/domain/recording/RecordingState.h"

// 순수 app 레이어 — Qt/FFmpeg include 없음.
// 채널별 녹화 수명(시작/중지/세그먼트 롤오버)을 관리한다.
// 경로 생성: IClock으로 타임스탬프를 받아 순수 C++ stdlib(chrono/ctime)로 조립.
//            Qt 의존 RecordingPaths는 infra이므로 이 클래스가 직접 사용하지 않는다.
namespace nv::app {

using RecordingObserver =
    std::function<void(const std::string& channelId, nv::domain::RecordingState)>;

class RecordingController {
public:
    RecordingController(IRecordingSink& sink,
                        IClock& clock,
                        ILogger& logger,
                        nv::domain::SegmentPolicy policy = {});

    // 토글: Idle이면 녹화 시작, Recording/Stopping이면 녹화 중지.
    // channelName은 파일명에 사용된다(채널 표시명).
    void toggle(const std::string& channelId, const std::string& channelName);

    // 재연결 알림: 녹화 중이던 채널이면 현재 세그먼트를 중지하고 새 세그먼트를 시작.
    // (분리 정책 splitOnReconnect가 false면 아무 동작 안 함.)
    void onReconnect(const std::string& channelId, const std::string& channelName);

    // 주기 호출: maxDuration 초과 채널의 세그먼트를 롤오버(stop + 새 경로로 start).
    void tick();

    nv::domain::RecordingState stateOf(const std::string& channelId) const;

    // UI 표시용 옵저버 — 상태 변화 시 호출된다.
    void setObserver(RecordingObserver observer);

private:
    struct ChannelRec {
        nv::domain::RecordingState state{nv::domain::RecordingState::Idle};
        std::string channelName;                            // 롤오버 시 재사용
        std::chrono::steady_clock::time_point segmentStart; // 현재 세그먼트 시작 시각
    };

    // outputPath 생성: <baseDir>/<channelName>_<yyyyMMdd_HHmmss>.mkv
    // baseDir은 환경변수 NV_RECORD_DIR or 현재 작업 디렉토리(테스트 친화적).
    std::string makePath(const std::string& channelName) const;

    void doStart(const std::string& channelId, const std::string& channelName);
    void doStop(const std::string& channelId);
    void notify(const std::string& channelId, nv::domain::RecordingState s);

    IRecordingSink&              m_sink;
    IClock&                      m_clock;
    ILogger&                     m_logger;
    nv::domain::SegmentPolicy    m_policy;
    std::map<std::string, ChannelRec> m_channels;
    RecordingObserver            m_observer;
};

} // namespace nv::app
