#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <QThreadPool>
#include "src/app/ports/IChannelRuntimeFactory.h"
#include "src/app/ports/IExecutor.h"
#include "src/app/ports/IFrameSurfaceRegistry.h"
#include "src/app/ports/IRecordingSink.h"
#include "src/app/ports/ISnapshotSink.h"
#include "src/app/MarshallingStreamSource.h"
#include "FfmpegStreamSource.h"
#include "src/infra/video/LatestSurfaceSlot.h"

namespace nv::infra {

// 채널별 (FfmpegStreamSource + LatestSurfaceSlot + Marshalling) 번들 생성.
// 슬롯은 레지스트리로 보관 — UI(타일)가 channelId로 조회한다.
// IFrameSurfaceRegistry 구현 — GridView/렌더러는 이 포트에만 의존(부채 #6 해소).
// 슬롯 수명: destroySource 후에도 즉시 파괴하지 않고 보관(타일 폴링 경합 방지),
//            동일 id 재생성 시 재사용. (M2a 단순화 — 채널 최대 20개라 누수 아님)
//
// M3 Task3 경계: IRecordingSink/ISnapshotSink를 구현해 app(RecordingController/
// SnapshotService, Task4)이 channelId 수준 명령을 내리면 여기서 infra로 위임한다.
//   - 녹화: 살아있는 채널 소스(FfmpegStreamSource)에 start/stopRecording 위임. AVPacket
//           배선은 FfmpegStreamSource 내부에서만(포트는 AVPacket 비노출).
//   - 스냅샷: 채널 슬롯의 최신 RGBA → PngSnapshotWriter. RGBA가 비면 false.
class ChannelSourceFactory final : public nv::app::IChannelRuntimeFactory,
                                   public nv::app::IFrameSurfaceRegistry,
                                   public nv::app::IRecordingSink,
                                   public nv::app::ISnapshotSink {
public:
    explicit ChannelSourceFactory(nv::app::IExecutor& executor) : m_executor(executor) {}

    std::unique_ptr<nv::app::IStreamSource> createSource(const std::string& channelId) override;
    void destroySource(const std::string& channelId) override;

    // IFrameSurfaceRegistry — UI/렌더러가 최신 서피스를 channelId로 조회.
    bool latestSurface(const std::string& channelId, nv::app::FrameSurface& out,
                       uint64_t lastSeq) override;
    void releaseConsumed(const std::string& channelId, void* handle) override;

    // IRecordingSink — app이 채널 녹화 시작/중지를 명령(살아있는 소스에 위임).
    bool startRecording(const std::string& channelId,
                        const std::string& outputPath) override;
    void stopRecording(const std::string& channelId) override;
    bool isRecording(const std::string& channelId) const override;
    bool hasRecordingError(const std::string& channelId) const override;

    // ISnapshotSink — 채널 슬롯의 최신 RGBA를 outputPath에 PNG로 저장.
    bool snapshot(const std::string& channelId, const std::string& outputPath) override;

    LatestSurfaceSlot* slot(const std::string& channelId);   // UI 조회용 (없으면 nullptr)
    std::vector<std::string> slotIds();                      // 통계용 슬롯 키 목록 (뮤텍스 보호)

private:
    // 소유권을 한 덩어리로: Marshalling(외피) ← Ffmpeg(내부) ← slot(레지스트리 소유)
    // Bundle은 생성/소멸 시 팩토리의 m_bundles에 자기를 등록/해제한다 — 소멸 시 즉시
    // 해제하므로 ChannelManager의 erase→destroySource 순서와 무관하게 dangling이 없다.
    class Bundle final : public nv::app::IStreamSource {
    public:
        Bundle(ChannelSourceFactory& owner, std::string channelId,
               LatestSurfaceSlot& slot, nv::app::IExecutor& ex)
            : m_owner(owner), m_channelId(std::move(channelId)),
              m_ffmpeg(slot), m_marshalled(m_ffmpeg, ex) {
            std::lock_guard lk(m_owner.m_mu);
            m_owner.m_bundles[m_channelId] = this;
        }
        ~Bundle() override {
            std::lock_guard lk(m_owner.m_mu);
            auto it = m_owner.m_bundles.find(m_channelId);
            if (it != m_owner.m_bundles.end() && it->second == this) {
                m_owner.m_bundles.erase(it);
            }
        }
        void open(const std::string& url, nv::app::StreamSourceListener& l) override {
            m_marshalled.open(url, l);
        }
        void close() override { m_marshalled.close(); }

        // 녹화 위임 대상 — 내부 FfmpegStreamSource(스레드 안전 메서드).
        FfmpegStreamSource& ffmpeg() { return m_ffmpeg; }

    private:
        ChannelSourceFactory& m_owner;
        std::string m_channelId;
        FfmpegStreamSource m_ffmpeg;
        nv::app::MarshallingStreamSource m_marshalled;
    };

    nv::app::IExecutor& m_executor;
    mutable std::mutex m_mu;          // slot()은 UI 스레드, create/destroy는 control 스레드
    std::map<std::string, std::unique_ptr<LatestSurfaceSlot>> m_slots;
    // 살아있는 채널 소스(녹화 위임용). createSource에서 등록, destroySource에서 제거.
    // 소유는 호출자(ChannelController)가 unique_ptr로 보유하므로 여기는 비소유 포인터.
    //
    // 스레드 계약(D3): IRecordingSink/ISnapshotSink 메서드(startRecording/stopRecording/
    // isRecording/hasRecordingError/snapshot)는 control 스레드에서만 호출된다는 불변량에
    // 의존한다. 이 메서드들은 m_mu 하에서 Bundle*를 읽고 락을 푼 뒤 b->ffmpeg()를 호출하는데,
    // Bundle 소멸(destroySource→호출자 unique_ptr 파괴)도 같은 control 스레드라 호출 도중
    // Bundle이 파괴될 수 없다 → 뮤텍스 밖 역참조가 안전하다(부채 D3 근거).
    std::map<std::string, Bundle*> m_bundles;

    // H3: 스냅샷 워커 풀 — 최대 2스레드로 제한(연타 시 스레드·버퍼 누적 방지).
    // 앱 종료 시 waitForDone()이 자동 호출돼 미완료 워커를 안전하게 대기한다.
    QThreadPool m_snapshotPool;
};

} // namespace nv::infra
