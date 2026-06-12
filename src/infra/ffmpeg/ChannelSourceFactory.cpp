#include "ChannelSourceFactory.h"

#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

#include "src/infra/persist/PngSnapshotWriter.h"

namespace nv::infra {

std::unique_ptr<nv::app::IStreamSource> ChannelSourceFactory::createSource(
    const std::string& channelId) {
    LatestSurfaceSlot* slot = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto& s = m_slots[channelId];
        if (!s) s = std::make_unique<LatestSurfaceSlot>();
        slot = s.get();
    }
    // Bundle 생성자가 m_bundles에 자기를 등록한다(소멸 시 자동 해제).
    return std::make_unique<Bundle>(*this, channelId, *slot, m_executor);
}

void ChannelSourceFactory::destroySource(const std::string& channelId) {
    // 슬롯 객체는 보존(폴링 안전 — UI 스레드의 latestSurface() 경합 방지),
    // GPU/RGBA 자원만 해제 — ~11MB/채널 누수 방지 (부채 #8 해소).
    // clear()는 슬롯 자체 뮤텍스로 보호되므로 UI 스레드와 안전.
    // clear 직후 UI가 latest()하면 seq==0이라 false 반환(정상 동작).
    LatestSurfaceSlot* s = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto it = m_slots.find(channelId);
        if (it != m_slots.end()) s = it->second.get();
    }
    if (s != nullptr) s->clear();
}

bool ChannelSourceFactory::latestSurface(const std::string& channelId,
                                         nv::app::FrameSurface& out, uint64_t lastSeq) {
    LatestSurfaceSlot* s = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto it = m_slots.find(channelId);
        if (it == m_slots.end()) return false;
        s = it->second.get();
    }
    return s->latest(out, lastSeq);   // 슬롯 자체 뮤텍스로 보호
}

void ChannelSourceFactory::releaseConsumed(const std::string& channelId, void* handle) {
    LatestSurfaceSlot* s = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto it = m_slots.find(channelId);
        if (it == m_slots.end()) return;
        s = it->second.get();
    }
    s->releaseConsumed(handle);
}

bool ChannelSourceFactory::startRecording(const std::string& channelId,
                                          const std::string& outputPath) {
    Bundle* b = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto it = m_bundles.find(channelId);
        if (it != m_bundles.end()) b = it->second;
    }
    if (b == nullptr) return false;   // 살아있는 소스 없음(미생성/이미 제거)
    return b->ffmpeg().startRecording(outputPath);
}

void ChannelSourceFactory::stopRecording(const std::string& channelId) {
    Bundle* b = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto it = m_bundles.find(channelId);
        if (it != m_bundles.end()) b = it->second;
    }
    if (b != nullptr) b->ffmpeg().stopRecording();
}

bool ChannelSourceFactory::isRecording(const std::string& channelId) const {
    Bundle* b = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto it = m_bundles.find(channelId);
        if (it != m_bundles.end()) b = it->second;
    }
    return b != nullptr && b->ffmpeg().isRecording();
}

bool ChannelSourceFactory::snapshot(const std::string& channelId,
                                    const std::string& outputPath) {
    LatestSurfaceSlot* s = nullptr;
    {
        std::lock_guard lk(m_mu);
        auto it = m_slots.find(channelId);
        if (it != m_slots.end()) s = it->second.get();
    }
    if (s == nullptr) {
        std::fprintf(stderr, "[ChannelSourceFactory] snapshot: 슬롯 없음 (%s)\n",
                     channelId.c_str());
        return false;
    }
    // 슬롯의 최신 RGBA(오버레이 없는 디코딩 원본). lastSeq=0이라 항상 현재 프레임을 받는다.
    // 순수 GPU 경로로 RGBA가 비면 width/height가 0이거나 rgba가 비어 false.
    // latest()가 frame.rgba에 깊은 복사를 채우므로 이 시점에서 슬롯 버퍼와는 분리돼 있다.
    LatestSurfaceSlot::Frame frame;
    if (!s->latest(frame, 0) || frame.rgba.empty() || frame.width <= 0 || frame.height <= 0) {
        std::fprintf(stderr, "[ChannelSourceFactory] snapshot: 프레임 RGBA 없음 (%s)\n",
                     channelId.c_str());
        return false;
    }

    // D4 비블로킹: PNG 압축(QImage::copy + 인코딩 + 디스크 쓰기)은 무겁다 — control 스레드에서
    // 직접 하면 전 채널 tick이 정지한다. RGBA 복사는 이미 latest()에서 끝났으므로, 인코딩/저장은
    // 별도 워커 스레드로 떼어낸다(detach). frame.rgba를 워커로 move해 호출자 버퍼와 분리한다.
    // 반환값은 "디스패치 성공"이라 항상 true — 실제 저장 실패는 워커가 stderr 로그로 남긴다.
    // (FilePanel의 QFileSystemWatcher가 저장 완료를 잡아 목록을 갱신한다.)
    std::thread([path = outputPath, w = frame.width, h = frame.height,
                 rgba = std::move(frame.rgba)]() mutable {
        PngSnapshotWriter::write(path, w, h, rgba.data());
    }).detach();
    return true;
}

LatestSurfaceSlot* ChannelSourceFactory::slot(const std::string& channelId) {
    std::lock_guard lk(m_mu);
    auto it = m_slots.find(channelId);
    return it == m_slots.end() ? nullptr : it->second.get();
}

std::vector<std::string> ChannelSourceFactory::slotIds() {
    std::lock_guard lk(m_mu);
    std::vector<std::string> ids;
    ids.reserve(m_slots.size());
    for (const auto& [id, _] : m_slots) ids.push_back(id);
    return ids;
}

} // namespace nv::infra
