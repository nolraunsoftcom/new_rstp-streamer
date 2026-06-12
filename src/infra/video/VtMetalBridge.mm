#include "VtMetalBridge.h"

#if defined(__APPLE__)

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

// ── ARC 처리 선택 ────────────────────────────────────────────────────────────
//   이 .mm은 별도 -fno-objc-arc 플래그 없이 빌드한다(CMake 변경 최소화).
//   CVMetalTextureRef / CVMetalTextureCacheRef는 CoreFoundation(CF) 타입이므로
//   ARC가 수명을 관리하지 않는다 — CFRetain/CFRelease로 직접 균형을 맞춘다(ARC 무관).
//   여기서는 map()이 만든 CVMetalTextureRef를 out.lumaRef/chromaRef로 그대로 넘겨
//   소비자가 unmap()에서 CFRelease하게 한다(create=+1, release=-1 균형).
//   CVMetalTextureGetTexture()가 돌려주는 id<MTLTexture>는 그 CVMetalTexture가
//   소유(+0)하므로 별도 retain 없이 (__bridge void*)로만 보관한다 — CVMetalTextureRef가
//   살아 있는 동안만 유효(헤더의 수명 계약 참조). __bridge는 소유권을 옮기지 않으므로
//   ARC/비ARC 양쪽에서 안전하다.

namespace nv::infra {

VtMetalBridge::~VtMetalBridge() {
    if (m_cache != nullptr) {
        CFRelease(static_cast<CVMetalTextureCacheRef>(m_cache));
        m_cache = nullptr;
    }
}

bool VtMetalBridge::init(void* mtlDevice) {
    if (mtlDevice == nullptr) return false;
    if (m_cache != nullptr) return true;  // 이미 초기화

    id<MTLDevice> device = (__bridge id<MTLDevice>)mtlDevice;
    CVMetalTextureCacheRef cache = nullptr;
    CVReturn rc = CVMetalTextureCacheCreate(
        kCFAllocatorDefault,
        nullptr,        // cacheAttributes
        device,
        nullptr,        // textureAttributes
        &cache);
    if (rc != kCVReturnSuccess || cache == nullptr) {
        return false;
    }
    m_cache = cache;
    return true;
}

bool VtMetalBridge::map(void* cvPixelBuffer, PlaneTextures& out) {
    if (m_cache == nullptr || cvPixelBuffer == nullptr) return false;

    CVPixelBufferRef pb = static_cast<CVPixelBufferRef>(cvPixelBuffer);
    const OSType fmt = CVPixelBufferGetPixelFormatType(pb);

    // NV12 = 4:2:0 2-plane(Y + interleaved CbCr). video-range / full-range 둘 다 처리.
    // range 자체는 셰이더(Task3) 몫 — 여기서는 플래그만 기록.
    bool fullRange = false;
    if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        fullRange = false;
    } else if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        fullRange = true;
    } else {
        return false;  // NV12 아님 — 폴백(상위가 RGBA 경로로)
    }

    CVMetalTextureCacheRef cache = static_cast<CVMetalTextureCacheRef>(m_cache);

    // plane 0: luma Y → R8Unorm (전체 크기)
    const size_t lumaW = CVPixelBufferGetWidthOfPlane(pb, 0);
    const size_t lumaH = CVPixelBufferGetHeightOfPlane(pb, 0);
    CVMetalTextureRef lumaCv = nullptr;
    CVReturn rcY = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, cache, pb, nullptr,
        MTLPixelFormatR8Unorm, lumaW, lumaH, /*planeIndex=*/0, &lumaCv);
    if (rcY != kCVReturnSuccess || lumaCv == nullptr) {
        if (lumaCv != nullptr) CFRelease(lumaCv);
        return false;
    }

    // plane 1: chroma CbCr → RG8Unorm (절반 크기)
    const size_t chromaW = CVPixelBufferGetWidthOfPlane(pb, 1);
    const size_t chromaH = CVPixelBufferGetHeightOfPlane(pb, 1);
    CVMetalTextureRef chromaCv = nullptr;
    CVReturn rcC = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, cache, pb, nullptr,
        MTLPixelFormatRG8Unorm, chromaW, chromaH, /*planeIndex=*/1, &chromaCv);
    if (rcC != kCVReturnSuccess || chromaCv == nullptr) {
        CFRelease(lumaCv);                       // 앞서 만든 luma ref 정리
        if (chromaCv != nullptr) CFRelease(chromaCv);
        return false;
    }

    id<MTLTexture> lumaTex = CVMetalTextureGetTexture(lumaCv);
    id<MTLTexture> chromaTex = CVMetalTextureGetTexture(chromaCv);
    if (lumaTex == nil || chromaTex == nil) {
        CFRelease(lumaCv);
        CFRelease(chromaCv);
        return false;
    }

    out.lumaTex = (__bridge void*)lumaTex;       // +0, lumaCv가 소유 — unmap 전까지 유효
    out.chromaTex = (__bridge void*)chromaTex;
    out.lumaRef = lumaCv;                         // +1, unmap에서 CFRelease
    out.chromaRef = chromaCv;
    out.width = static_cast<int>(lumaW);
    out.height = static_cast<int>(lumaH);
    out.fullRange = fullRange;
    return true;
}

void VtMetalBridge::unmap(PlaneTextures& planes) {
    if (planes.lumaRef != nullptr) {
        CFRelease(static_cast<CVMetalTextureRef>(planes.lumaRef));
        planes.lumaRef = nullptr;
    }
    if (planes.chromaRef != nullptr) {
        CFRelease(static_cast<CVMetalTextureRef>(planes.chromaRef));
        planes.chromaRef = nullptr;
    }
    planes.lumaTex = nullptr;
    planes.chromaTex = nullptr;
}

void VtMetalBridge::flush() {
    if (m_cache != nullptr) {
        CVMetalTextureCacheFlush(static_cast<CVMetalTextureCacheRef>(m_cache), 0);
    }
}

} // namespace nv::infra

#else  // !__APPLE__ — 비macOS 스텁(심볼 존재, 전부 no-op/false).

namespace nv::infra {

VtMetalBridge::~VtMetalBridge() = default;
bool VtMetalBridge::init(void*) { return false; }
bool VtMetalBridge::map(void*, PlaneTextures&) { return false; }
void VtMetalBridge::unmap(PlaneTextures&) {}
void VtMetalBridge::flush() {}

} // namespace nv::infra

#endif  // __APPLE__
