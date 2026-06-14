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

    // 토글: Idle이면 녹화 시작(armed=true), Recording/armed이면 녹화 중지(armed=false).
    // channelName은 파일명에 사용된다(채널 표시명).
    void toggle(const std::string& channelId, const std::string& channelName);

    // 재연결(드롭 엣지) 알림: 소스가 곧 close되는 시점이므로 새 세그먼트를 시작하지 않는다.
    // armed && Recording이면 현재 세그먼트만 종료(doStop → Idle)하되 armed는 유지한다.
    // 복구 시 onStreaming이 새 세그먼트를 시작한다. (splitOnReconnect=false면 무동작.)
    void onReconnect(const std::string& channelId, const std::string& channelName);

    // 복구(Streaming 재도달) 알림: armed && Idle이면 새 세그먼트를 시작(doStart).
    // 이미 Recording이면 무동작(중복 방지). 끊김→복구마다 녹화를 지속시킨다.
    void onStreaming(const std::string& channelId, const std::string& channelName);

    // 채널 삭제 알림: 녹화 중이면 내부 상태를 Idle로 정리해 유령 Recording 방지.
    // sink.stopRecording은 best-effort(소스가 곧 파괴됨). m_channels에서 항목 제거.
    void onChannelRemoved(const std::string& channelId);

    // 주기 호출: maxDuration 초과 채널의 세그먼트를 롤오버(stop + 새 경로로 start).
    void tick();

    nv::domain::RecordingState stateOf(const std::string& channelId) const;

    // 직전에 마감된 세그먼트의 파일 경로(없으면 빈 문자열). Recording→Idle 전이 시점에
    // notify() 내부에서 갱신된다 — 옵저버가 같은 호출에서 안전하게 조회할 수 있다.
    std::string lastSegmentPath(const std::string& channelId) const;
    // 직전에 마감된 세그먼트의 길이(초). 시작~중지 시각차(IClock 기준). 없으면 0.
    int lastSegmentDurationSec(const std::string& channelId) const;

    // UI 표시용 옵저버 — 상태 변화 시 호출된다.
    void setObserver(RecordingObserver observer);

private:
    struct ChannelRec {
        nv::domain::RecordingState state{nv::domain::RecordingState::Idle};
        bool armed{false};                                  // 사용자 녹화 의도(끊김에도 유지)
        std::string channelName;                            // 롤오버 시 재사용
        std::chrono::steady_clock::time_point segmentStart; // 현재 세그먼트 시작 시각
        std::string currentPath;                            // 활성 세그먼트 파일 경로(doStart에서 설정, Idle 캡처 후 비움)
        std::string lastPath;                               // 직전 마감 세그먼트 경로(토스트 표시용)
        int lastDurationSec{0};                             // 직전 마감 세그먼트 길이(초)
        // D2 백오프: doStart 연속 실패 횟수. kMaxStartFailures 도달 시 armed를 내려
        // 무한 재시도·로그 스팸을 막는다. doStart 성공 시 0으로 리셋.
        unsigned startFailures{0};
        // D1 재시도 의도: 롤오버/디스크오류로 armed && Idle이 됐을 때만 tick에서 doStart를
        // 재시도하게 하는 게이트. onReconnect의 드롭 엣지(소스가 죽어가는 중, onStreaming
        // 복구를 기다림)에서는 false라 tick이 죽은 소스에 재시도하지 않는다(백오프 오염 방지).
        bool retryStart{false};
        // D10 백오프: 디스크/쓰기 오류로 연속 수렴(hasRecordingError)된 횟수. kMaxDiskErrors
        // 도달 시 armed를 내려 무한 파일/스레드 churn을 멈춘다(디스크 풀 등). 확인된 정상
        // 세그먼트(grace 경과·isRecording·무오류)에서 0으로 리셋된다. startFailures(동기
        // startRecording==false 경로)와 별개 카운터 — 디스크오류는 비동기 쓰기 시점에 나기 때문.
        unsigned diskErrors{0};
    };

    // D2: doStart 연속 실패가 이 횟수에 도달하면 armed=false + 1회 경고(사용자 재시도 필요).
    static constexpr unsigned kMaxStartFailures = 5;
    // D10: 디스크/쓰기 오류 연속 수렴이 이 횟수에 도달하면 armed=false + 1회 경고(디스크 확인 필요).
    static constexpr unsigned kMaxDiskErrors = 5;
    // D10: 디스크오류 백오프 리셋은 "지속된 정상 녹화"에서만 일어나야 한다. avio 버퍼링 탓에
    // 가득 찬 디스크에서도 새 세그먼트가 열린 직후 수초간 isRecording==true·무오류 윈도우가
    // 생긴다 — 이 짧은 윈도우(grace 3s)에 리셋되면 diskErrors가 임계에 못 가 무한 churn이
    // 된다(실측: 30+ 0바이트 파일, disarm 0회). 버퍼 플러시/오류표면화 간격보다 충분히 큰
    // 윈도우를 둬 진짜 지속 녹화만 리셋한다.
    static constexpr std::chrono::seconds kDiskHealthyResetWindow{30};

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
    // D2: 같은 초 stop→start 경로 충돌 방지용 단조 시퀀스(makePath가 const라 mutable).
    // control 스레드 단일 호출이므로 atomic 불필요.
    mutable unsigned             m_pathSeq{0};
};

} // namespace nv::app
