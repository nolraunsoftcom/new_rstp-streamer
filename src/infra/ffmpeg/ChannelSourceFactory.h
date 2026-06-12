#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "src/app/ports/IChannelRuntimeFactory.h"
#include "src/app/ports/IExecutor.h"
#include "src/app/MarshallingStreamSource.h"
#include "FfmpegStreamSource.h"
#include "src/infra/video/LatestFrameSlot.h"

namespace nv::infra {

// 채널별 (FfmpegStreamSource + LatestFrameSlot + Marshalling) 번들 생성.
// 슬롯은 레지스트리로 보관 — UI(타일)가 channelId로 조회한다.
// 슬롯 수명: destroySource 후에도 즉시 파괴하지 않고 보관(타일 폴링 경합 방지),
//            동일 id 재생성 시 재사용. (M2a 단순화 — 채널 최대 20개라 누수 아님)
class ChannelSourceFactory final : public nv::app::IChannelRuntimeFactory {
public:
    explicit ChannelSourceFactory(nv::app::IExecutor& executor) : m_executor(executor) {}

    std::unique_ptr<nv::app::IStreamSource> createSource(const std::string& channelId) override;
    void destroySource(const std::string& channelId) override;

    LatestFrameSlot* slot(const std::string& channelId);   // UI 조회용 (없으면 nullptr)
    std::vector<std::string> slotIds();                    // 통계용 슬롯 키 목록 (뮤텍스 보호)

private:
    // 소유권을 한 덩어리로: Marshalling(외피) ← Ffmpeg(내부) ← slot(레지스트리 소유)
    class Bundle final : public nv::app::IStreamSource {
    public:
        Bundle(LatestFrameSlot& slot, nv::app::IExecutor& ex)
            : m_ffmpeg(slot), m_marshalled(m_ffmpeg, ex) {}
        void open(const std::string& url, nv::app::StreamSourceListener& l) override {
            m_marshalled.open(url, l);
        }
        void close() override { m_marshalled.close(); }

    private:
        FfmpegStreamSource m_ffmpeg;
        nv::app::MarshallingStreamSource m_marshalled;
    };

    nv::app::IExecutor& m_executor;
    std::mutex m_mu;                 // slot()은 UI 스레드, create/destroy는 control 스레드
    std::map<std::string, std::unique_ptr<LatestFrameSlot>> m_slots;
};

} // namespace nv::infra
