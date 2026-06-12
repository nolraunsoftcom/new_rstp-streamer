#pragma once
#include <string>

// 순수 app 포트 — Qt/FFmpeg include 없음.
// app 레이어가 infra에 "이 채널 녹화 시작/중지"를 명령하는 인터페이스.
// AVPacket 비노출: 실제 패킷 배선(FfmpegStreamSource → FfmpegRecorder)은
// infra 내부에서만 이루어진다.
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
};

} // namespace nv::app
