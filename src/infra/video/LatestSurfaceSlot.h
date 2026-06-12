#pragma once
#include <cstdint>
#include <mutex>
#include <vector>
#include "src/app/ports/IFrameSurface.h"

namespace nv::infra {

// LatestFrameSlot 후속 — 디코드 스레드 → UI 스레드 최신 서피스 메일박스 (설계 M2b Task4).
// 백프레셔 정책: 항상 마지막 1장만 유지 (표시는 최신만 의미 있음).
// CPU 변종(RGBA tight, stride=width*4)과 GPU 변종(VideoToolbox CVPixelBufferRef)을 모두 담는다.
//
// ── GPU 핸들 수명 계약 (선택: "현재 1개 소유 + 핸드오프 시 추가 retain") ──────────────
//   * publishGpu(w,h,handle): 슬롯이 handle을 CVPixelBufferRetain로 1회 retain해 소유한다.
//     직전에 보유하던 핸들은 CVPixelBufferRelease로 즉시 해제한다(항상 ≤1개 보유).
//   * latest(FrameSurface&, seq): GpuTexture를 넘길 때, 넘기는 핸들을 한 번 더 retain한 뒤
//     out.gpuHandle에 담는다. **소비자(렌더러)는 그리기를 마치면 그 핸들을 반드시 release**
//     해야 한다(소비자 소유의 추가 ref). 이렇게 하면 슬롯이 다음 프레임으로 교체되어도
//     소비자가 들고 있는 ref가 살아 있어 그리는 동안 안전하다.
//   * retain/release는 CoreVideo 호출 — __APPLE__ 가드 하에서 .cpp가 아닌 헤더에 inline 구현
//     불가하므로(Obj-C/CoreVideo 의존), 이 헤더는 void* 핸들과 함수 포인터(retainer/releaser)를
//     주입받는 방식으로 플랫폼 의존을 인프라 .cpp(FfmpegStreamSource)로 격리한다.
//
// ── 이 태스크(Task4)의 안전 선택 ──────────────────────────────────────────────
//   렌더러는 아직 GPU 핸들을 소비하지 않는다(Task5). 따라서 화면이 나오도록
//   FfmpegStreamSource가 hw 디코드 성공 시에도 av_hwframe_transfer_data로 CPU RGBA를 만들어
//   publishGpu와 함께 RGBA도 채운다(아래 publishGpu의 rgba 인자). latest()는 RGBA를 항상
//   복사해 넘기므로 기존 SW 렌더러가 그대로 그린다. Task5에서 gpuHandle 직행으로 최적화.
//
// 스레드 안전: 모든 public 메서드는 m_mu로 보호된다.
class LatestSurfaceSlot {
public:
    // CVPixelBufferRetain/Release 시그니처 (CoreVideo). 플랫폼 .cpp에서 주입.
    using RetainFn = void* (*)(void*);
    using ReleaseFn = void (*)(void*);

    // 기존 LatestFrameSlot::Frame 호환 — RGBA 폴링 소비자(VideoTileWidget/SoakLogger)용.
    struct Frame {
        int width = 0;
        int height = 0;
        uint64_t seq = 0;
        std::vector<uint8_t> rgba;
    };

    LatestSurfaceSlot() = default;
    ~LatestSurfaceSlot() { releaseGpuLocked(nullptr); }

    LatestSurfaceSlot(const LatestSurfaceSlot&) = delete;
    LatestSurfaceSlot& operator=(const LatestSurfaceSlot&) = delete;

    // 슬롯이 GPU 핸들의 retain/release를 수행할 함수 포인터를 1회 설정한다(소스 open 시).
    void setGpuRefcounters(RetainFn retain, ReleaseFn release) {
        std::lock_guard lk(m_mu);
        m_retain = retain;
        m_release = release;
    }

    // SW 폴백/테스트: CPU RGBA만 발행.
    void publishCpu(int width, int height, const uint8_t* rgbaTight) {
        std::lock_guard lk(m_mu);
        releaseGpuLocked(nullptr);            // 이전 GPU 핸들 정리
        m_kind = nv::app::FrameSurface::Kind::CpuRgba;
        m_width = width;
        m_height = height;
        m_rgba.assign(rgbaTight, rgbaTight + static_cast<size_t>(width) * height * 4);
        ++m_seq;
    }

    // HW 경로: GPU 핸들(CVPixelBufferRef)을 소유 retain. 동반 RGBA(폴백 표시용)도 함께 담는다.
    // rgbaTight가 nullptr이면 RGBA는 비운다(순수 GPU 경로 — Task5 이후).
    void publishGpu(int width, int height, void* handle,
                    const uint8_t* rgbaTight) {
        std::lock_guard lk(m_mu);
        releaseGpuLocked(nullptr);            // 이전 GPU 핸들 정리 (항상 ≤1개)
        if (handle != nullptr && m_retain != nullptr) {
            m_gpuHandle = m_retain(handle);   // 슬롯 소유 ref 1개 획득
        } else {
            m_gpuHandle = handle;             // refcounter 미설정 시 그대로(테스트 등)
        }
        m_kind = nv::app::FrameSurface::Kind::GpuTexture;
        m_width = width;
        m_height = height;
        if (rgbaTight != nullptr) {
            m_rgba.assign(rgbaTight, rgbaTight + static_cast<size_t>(width) * height * 4);
        } else {
            m_rgba.clear();
        }
        ++m_seq;
    }

    // IFrameSurfaceRegistry 위임용 — lastSeq보다 새 서피스가 있으면 out에 채우고 true.
    // GpuTexture면 out.gpuHandle은 추가 retain된 ref (소비자가 release 책임).
    bool latest(nv::app::FrameSurface& out, uint64_t lastSeq) {
        std::lock_guard lk(m_mu);
        if (m_seq == 0 || m_seq == lastSeq) return false;
        out.kind = m_kind;
        out.width = m_width;
        out.height = m_height;
        out.seq = m_seq;
        out.rgba = m_rgba;
        if (m_kind == nv::app::FrameSurface::Kind::GpuTexture && m_gpuHandle != nullptr &&
            m_retain != nullptr) {
            out.gpuHandle = m_retain(m_gpuHandle);   // 소비자 소유 추가 ref
        } else {
            out.gpuHandle = m_gpuHandle;             // refcounter 없으면 비소유 포인터(주의)
        }
        return true;
    }

    // 채널 영구 삭제 시 GPU/RGBA 자원을 반납한다 (~11MB/채널 누수 방지).
    // 슬롯 객체 자체는 파괴하지 않는다 — UI 폴링 race 방지 불변량 유지.
    // 이후 latest()는 seq==0으로 false를 반환하므로 소비자에 안전.
    void clear() {
        std::lock_guard lk(m_mu);
        releaseGpuLocked(nullptr);   // GPU 핸들 해제
        m_rgba.clear();
        m_rgba.shrink_to_fit();      // RGBA 힙 메모리 반납 (~11MB/채널)
        m_kind = nv::app::FrameSurface::Kind::None;
        m_seq = 0;
        m_width = 0;
        m_height = 0;
    }

    // 소비자가 latest()로 받은 GpuTexture 핸들을 다 그린 뒤 호출 — 슬롯이 건넨 추가 ref를 푼다.
    // (CoreVideo 의존을 슬롯에 격리하기 위한 헬퍼. refcounter 미설정이면 무동작.)
    void releaseConsumed(void* handle) {
        std::lock_guard lk(m_mu);
        if (handle != nullptr && m_release != nullptr) m_release(handle);
    }

    // 기존 RGBA 폴링 소비자 호환 — LatestFrameSlot::latest(Frame&, seq)와 동일 의미.
    bool latest(Frame& out, uint64_t lastSeq) const {
        std::lock_guard lk(m_mu);
        if (m_seq == 0 || m_seq == lastSeq) return false;
        out.width = m_width;
        out.height = m_height;
        out.seq = m_seq;
        out.rgba = m_rgba;
        return true;
    }

private:
    // 현재 GPU 핸들을 release하고 next로 교체(next는 이미 retain된 상태로 들어오거나 nullptr).
    void releaseGpuLocked(void* next) {
        if (m_gpuHandle != nullptr && m_release != nullptr) {
            m_release(m_gpuHandle);
        }
        m_gpuHandle = next;
    }

    mutable std::mutex m_mu;
    nv::app::FrameSurface::Kind m_kind = nv::app::FrameSurface::Kind::None;
    int m_width = 0;
    int m_height = 0;
    uint64_t m_seq = 0;
    std::vector<uint8_t> m_rgba;
    void* m_gpuHandle = nullptr;       // 슬롯 소유 ref (≤1개)
    RetainFn m_retain = nullptr;
    ReleaseFn m_release = nullptr;
};

} // namespace nv::infra
