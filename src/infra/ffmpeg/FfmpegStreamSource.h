#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include "src/app/ports/IStreamSource.h"
#include "src/infra/video/LatestFrameSlot.h"

namespace nv::infra {

// IStreamSource의 FFmpeg 구현 (설계 D4).
// open(): demux+decode 스레드를 띄운다. 리스너 콜백은 그 스레드에서 호출되므로
// 호출측은 MarshallingStreamSource로 감싸 control 스레드로 직렬화해야 한다.
// RTSP 전송은 TCP 강제 (설계 3차 리뷰 R4).
class FfmpegStreamSource final : public nv::app::IStreamSource {
public:
    explicit FfmpegStreamSource(LatestFrameSlot& frameSlot);
    ~FfmpegStreamSource() override;

    void open(const std::string& url, nv::app::StreamSourceListener& listener) override;
    void close() override;   // demux 스레드 합류까지 블로킹

private:
    void run(std::string url, nv::app::StreamSourceListener* listener);
    static int interruptCb(void* opaque);

    LatestFrameSlot& m_frameSlot;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<int64_t> m_graceUntilMs{0};  // close 중 TEARDOWN 송신 허용 데드라인 (steady ms)
};

} // namespace nv::infra
