#pragma once
#include <string>

// 순수 app 포트 — Qt/FFmpeg include 없음.
// app 레이어가 infra에 "이 채널 녹화 시작/중지"를 명령하는 인터페이스.
// AVPacket 비노출: 실제 패킷 배선(FfmpegStreamSource → FfmpegRecorder)은
// infra 내부에서만 이루어진다.
//
// 스레드 계약(D3): 이 인터페이스의 모든 메서드는 control 스레드(ControlExecutor)에서만
// 호출된다. const 메서드(isRecording/hasRecordingError)도 예외가 아니다. 구현체
// (ChannelSourceFactory)는 이 불변량에 의존해 m_bundles의 Bundle* 원시 포인터를
// 뮤텍스 밖에서 안전하게 역참조한다 — Bundle 생성/소멸(createSource/destroySource)도
// 같은 control 스레드라 호출 중 dangling이 발생하지 않는다.
namespace nv::app {

class IRecordingSink {
public:
    virtual ~IRecordingSink() = default;

    // channelId 채널의 녹화를 outputPath 경로로 시작한다.
    // 이미 녹화 중이면 false 반환. 성공 시 true.
    virtual bool startRecording(const std::string& channelId,
                                const std::string& outputPath) = 0;

    // channelId 채널의 녹화를 중지한다. 녹화 중이 아니면 무시.
    virtual void stopRecording(const std::string& channelId) = 0;

    // channelId 채널이 현재 녹화 중인지 반환한다.
    virtual bool isRecording(const std::string& channelId) const = 0;

    // channelId 채널의 레코더가 쓰기 오류(디스크 풀/쓰기 실패)를 만났는지 반환한다(D10).
    // 레코더가 없거나 정상이면 false. RecordingController가 reconcile에서 이를 확인해
    // 디스크 오류를 3초 내 가시화한다(REC 해제 + 경고). isRecording은 여전히 true일 수 있어
    // (m_open 유지) 별도 신호가 필요하다.
    virtual bool hasRecordingError(const std::string& channelId) const = 0;
};

} // namespace nv::app
