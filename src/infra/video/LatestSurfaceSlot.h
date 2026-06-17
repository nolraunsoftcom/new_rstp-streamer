#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>
#include "src/app/ports/IFrameSurface.h"

namespace nv::infra {

// LatestFrameSlot 후속 — 디코드 스레드 → UI 스레드 서피스 전달 (설계 M2b Task4 + jitter FIFO).
//
// ── jitter 평탄화 FIFO (depth N, 기본 2) ──────────────────────────────────────────
//   과거: depth-1 메일박스(최신 1장만 유지). 30Hz 표시 펄스 vs 소스 프레임레이트의 비트와
//         TCP burst가 그대로 노출돼 "0프레임 다음 2프레임" 형태의 미세 저더가 보였다.
//   현재: 작은 탄력 FIFO를 둬서 "0 다음 2"를 "1 다음 1"로 평탄화한다.
//     * 표시 경로 latest(FrameSurface&, lastSeq)는 seq > lastSeq 중 "가장 오래된" 엔트리를
//       반환한다(폴당 1프레임 전진 = 평탄화). **비파괴**라 같은 채널을 여러 소비자(그리드
//       타일 + 전체화면 위젯)가 각자 lastSeq 커서로 동시에 읽어도 안전하다.
//     * publish가 depth를 넘기면 가장 오래된 엔트리를 축출(핸들 release)한다 → 지연 상한이
//       depth 프레임으로 고정되고, 폴링이 멈춘 오프스크린 채널도 큐가 무한 성장하지 않는다.
//     * 추가 지연 = depth × 프레임간격(기본 2 ≈ 66ms@30fps). depth는 setDepth로 튜닝.
//   스냅샷/소크용 latest(Frame&, lastSeq)는 페이싱 없이 "최신"(큐 back)을 반환한다.
//
// CPU 변종(RGBA tight, stride=width*4)과 GPU 변종(VideoToolbox CVPixelBufferRef / D3D11
// AVFrame*)을 같은 엔트리에 담아 흐르게 한다.
//
// ── GPU 핸들 수명 계약 ("엔트리당 1 소유 + 핸드오프 시 추가 retain") ──────────────────
//   * publishGpu(w,h,handle): 새 엔트리가 handle을 retain로 1회 소유한다(엔트리당 ≤1개,
//     슬롯 보유 총합 ≤ depth개). 큐 축출/clear 시 그 엔트리의 핸들을 release한다.
//   * latest(FrameSurface&, seq): GpuTexture를 넘길 때 핸들을 한 번 더 retain해 out.gpuHandle에
//     담는다. **소비자(렌더러)는 그리기를 마치면 releaseConsumed로 그 핸들을 반드시 반납**
//     해야 한다(소비자 소유의 추가 ref). 비파괴 읽기라 슬롯/타 소비자의 ref와 독립적이다.
//   * retain/release는 플랫폼 의존(CoreVideo/AVFrame) — 함수 포인터로 주입받아(.cpp) 격리한다.
//
// 스레드 안전: 모든 public 메서드는 m_mu로 보호된다.
class LatestSurfaceSlot {
public:
    // CVPixelBufferRetain/Release (macOS) 또는 av_frame_clone/free (Windows) 시그니처.
    using RetainFn = void* (*)(void*);
    using ReleaseFn = void (*)(void*);

    // 기존 LatestFrameSlot::Frame 호환 — RGBA 폴링 소비자(스냅샷/SoakLogger)용.
    struct Frame {
        int width = 0;
        int height = 0;
        uint64_t seq = 0;
        std::vector<uint8_t> rgba;
    };

    LatestSurfaceSlot() = default;
    ~LatestSurfaceSlot() {
        for (auto& e : m_q) releaseHandle(e.gpuHandle);
    }

    LatestSurfaceSlot(const LatestSurfaceSlot&) = delete;
    LatestSurfaceSlot& operator=(const LatestSurfaceSlot&) = delete;

    // 슬롯이 GPU 핸들의 retain/release를 수행할 함수 포인터를 1회 설정한다(소스 open 시).
    void setGpuRefcounters(RetainFn retain, ReleaseFn release) {
        std::lock_guard lk(m_mu);
        m_retain = retain;
        m_release = release;
    }

    // jitter FIFO 깊이(표시 추가 지연 = depth 프레임). 최소 1. 줄이면 초과분을 즉시 축출한다.
    void setDepth(std::size_t depth) {
        std::lock_guard lk(m_mu);
        m_depth = depth < 1 ? 1 : depth;
        evictLocked();
    }

    // SW 폴백/테스트: CPU RGBA만 발행.
    void publishCpu(int width, int height, const uint8_t* rgbaTight) {
        std::lock_guard lk(m_mu);
        Entry e;
        e.kind = nv::app::FrameSurface::Kind::CpuRgba;
        e.width = width;
        e.height = height;
        e.rgba.assign(rgbaTight, rgbaTight + static_cast<size_t>(width) * height * 4);
        e.seq = ++m_seq;
        m_q.push_back(std::move(e));
        evictLocked();
    }

    // HW 경로: GPU 핸들을 엔트리가 소유 retain. 동반 RGBA(폴백 표시용)도 함께 담는다.
    // rgbaTight가 nullptr이면 RGBA는 비운다(순수 GPU 경로).
    void publishGpu(int width, int height, void* handle, const uint8_t* rgbaTight) {
        std::lock_guard lk(m_mu);
        Entry e;
        e.kind = nv::app::FrameSurface::Kind::GpuTexture;
        e.width = width;
        e.height = height;
        e.gpuHandle = (handle != nullptr && m_retain != nullptr) ? m_retain(handle) : handle;
        if (rgbaTight != nullptr) {
            e.rgba.assign(rgbaTight, rgbaTight + static_cast<size_t>(width) * height * 4);
        }
        e.seq = ++m_seq;
        m_q.push_back(std::move(e));
        evictLocked();
    }

    // 표시 경로(IFrameSurfaceRegistry 위임). lastSeq보다 새 엔트리 중 "가장 오래된" 것을 out에
    // 채우고 true(폴당 1프레임 전진 = jitter 평탄화). 비파괴 — 멀티 리더 안전.
    // GpuTexture면 out.gpuHandle은 추가 retain된 ref(소비자가 releaseConsumed 책임).
    bool latest(nv::app::FrameSurface& out, uint64_t lastSeq) {
        std::lock_guard lk(m_mu);
        const Entry* pick = nullptr;
        for (const auto& e : m_q) {   // deque는 seq 오름차순 — 첫 미소비 엔트리가 가장 오래됨
            if (e.seq > lastSeq) { pick = &e; break; }
        }
        if (pick == nullptr) return false;
        out.kind = pick->kind;
        out.width = pick->width;
        out.height = pick->height;
        out.seq = pick->seq;
        out.rgba = pick->rgba;
        if (pick->kind == nv::app::FrameSurface::Kind::GpuTexture && pick->gpuHandle != nullptr &&
            m_retain != nullptr) {
            out.gpuHandle = m_retain(pick->gpuHandle);   // 소비자 소유 추가 ref
        } else {
            out.gpuHandle = pick->gpuHandle;             // refcounter 없으면 비소유 포인터(주의)
        }
        return true;
    }

    // 채널 영구 삭제 시 GPU/RGBA 자원을 반납한다. 슬롯 객체 자체는 파괴하지 않는다
    // (UI 폴링 race 방지 불변량 유지). 이후 latest()는 seq==0으로 false를 반환하므로 안전.
    void clear() {
        std::lock_guard lk(m_mu);
        for (auto& e : m_q) releaseHandle(e.gpuHandle);
        m_q.clear();
        m_q.shrink_to_fit();   // RGBA 힙 메모리 반납 (~depth×8MB/채널)
        m_seq = 0;
    }

    // 소비자가 latest()로 받은 GpuTexture 핸들을 다 그린 뒤 호출 — 건넨 추가 ref를 푼다.
    void releaseConsumed(void* handle) {
        std::lock_guard lk(m_mu);
        if (handle != nullptr && m_release != nullptr) m_release(handle);
    }

    // 스냅샷/SoakLogger 호환 — 페이싱 없이 "최신" RGBA(큐 back)를 반환한다.
    bool latest(Frame& out, uint64_t lastSeq) const {
        std::lock_guard lk(m_mu);
        if (m_q.empty()) return false;
        const Entry& e = m_q.back();
        if (e.seq == 0 || e.seq == lastSeq) return false;
        out.width = e.width;
        out.height = e.height;
        out.seq = e.seq;
        out.rgba = e.rgba;
        return true;
    }

private:
    struct Entry {
        nv::app::FrameSurface::Kind kind = nv::app::FrameSurface::Kind::None;
        int width = 0;
        int height = 0;
        uint64_t seq = 0;
        std::vector<uint8_t> rgba;       // tight, stride = width*4 (GPU 순수 경로면 빈 벡터)
        void* gpuHandle = nullptr;       // 엔트리 소유 ref (≤1개)
    };

    void releaseHandle(void*& handle) {
        if (handle != nullptr && m_release != nullptr) m_release(handle);
        handle = nullptr;
    }

    // depth 초과분을 앞(가장 오래된)에서 축출하며 핸들을 반납한다.
    void evictLocked() {
        while (m_q.size() > m_depth) {
            releaseHandle(m_q.front().gpuHandle);
            m_q.pop_front();
        }
    }

    mutable std::mutex m_mu;
    std::deque<Entry> m_q;
    std::size_t m_depth = 2;            // jitter FIFO 기본 깊이 (~66ms@30fps)
    uint64_t m_seq = 0;                 // 단조 증가 — 발행마다 +1
    RetainFn m_retain = nullptr;
    ReleaseFn m_release = nullptr;
};

} // namespace nv::infra
