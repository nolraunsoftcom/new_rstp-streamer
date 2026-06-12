#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "src/app/ports/IRecordingSink.h"

namespace nv::test {

// startRecording/stopRecording/isRecording 호출을 기록하는 테스트 페이크.
class FakeRecordingSink final : public nv::app::IRecordingSink {
public:
    bool startRecording(const std::string& channelId,
                        const std::string& outputPath) override {
        if (m_recording[channelId]) return false;
        m_recording[channelId] = true;
        startCalls.push_back({channelId, outputPath});
        ++startCount;
        return true;
    }

    void stopRecording(const std::string& channelId) override {
        m_recording[channelId] = false;
        stopCalls.push_back(channelId);
        ++stopCount;
    }

    bool isRecording(const std::string& channelId) const override {
        auto it = m_recording.find(channelId);
        return it != m_recording.end() && it->second;
    }

    bool hasRecordingError(const std::string& channelId) const override {
        auto it = m_recordError.find(channelId);
        return it != m_recordError.end() && it->second;
    }

    // 테스트 헬퍼: 채널 쓰기 오류(디스크 풀 등) 모사 — D10 reconcile 검증용.
    void setRecordingError(const std::string& channelId, bool err) {
        m_recordError[channelId] = err;
    }

    struct StartCall { std::string channelId; std::string outputPath; };

    std::vector<StartCall> startCalls;
    std::vector<std::string> stopCalls;
    int startCount = 0;
    int stopCount  = 0;

private:
    std::unordered_map<std::string, bool> m_recording;
    std::unordered_map<std::string, bool> m_recordError;
};

} // namespace nv::test
