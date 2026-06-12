#include "HwContext.h"

namespace nv::infra {

namespace {
// 이 빌드가 타깃하는 hw 디바이스 타입과 hw 픽셀 포맷.
// macOS는 VideoToolbox만 동작 보장. Windows(D3D11VA)는 Task6에서 활성.
#if defined(__APPLE__)
constexpr AVHWDeviceType kHwType = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(_WIN32)
// Task6 스텁: 심볼은 존재하지만 실제 동작 검증은 Windows 브링업에서.
constexpr AVHWDeviceType kHwType = AV_HWDEVICE_TYPE_D3D11VA;
#else
constexpr AVHWDeviceType kHwType = AV_HWDEVICE_TYPE_NONE;
#endif
} // namespace

HwContext::~HwContext() {
    if (m_deviceCtx != nullptr) av_buffer_unref(&m_deviceCtx);
}

// 정본 hw_decode.c의 get_hw_format와 동일한 계약:
// 후보 목록(pix_fmts, -1 종료)에서 협상된 hw 포맷을 찾으면 반환.
// 없으면 SW 폴백을 위해 첫 번째(비-hw) 포맷을 반환한다.
AVPixelFormat HwContext::getFormat(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    auto* self = static_cast<HwContext*>(ctx->opaque);
    const AVPixelFormat hw = self != nullptr ? self->m_hwPixFmt : AV_PIX_FMT_NONE;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == hw) return *p;   // hw surface 포맷 선택
    }
    // hw 포맷이 후보에 없음 — SW 폴백 (libavcodec이 SW 디코더로 진행).
    return fmts[0];
}

bool HwContext::init(AVCodecContext* dec, const AVCodec* codec) {
    if (kHwType == AV_HWDEVICE_TYPE_NONE) return false;

    // 1) 이 코덱이 타깃 디바이스 타입을 hw_device_ctx 방식으로 지원하는지 찾는다.
    AVPixelFormat hwPix = AV_PIX_FMT_NONE;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (cfg == nullptr) break;   // 더 이상 구성 없음 → 미지원
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
            cfg->device_type == kHwType) {
            hwPix = cfg->pix_fmt;
            break;
        }
    }
    if (hwPix == AV_PIX_FMT_NONE) return false;   // 이 코덱+디바이스 조합 미지원

    // 2) hw 디바이스 컨텍스트 생성.
    AVBufferRef* devCtx = nullptr;
    if (av_hwdevice_ctx_create(&devCtx, kHwType, nullptr, nullptr, 0) < 0) {
        return false;   // 디바이스 없음/생성 실패 → SW 폴백
    }

    // 3) 디코더에 배선: get_format 콜백 + hw_device_ctx ref + opaque(self).
    m_deviceCtx = devCtx;
    m_hwPixFmt = hwPix;
    dec->opaque = this;
    dec->get_format = &HwContext::getFormat;
    dec->hw_device_ctx = av_buffer_ref(m_deviceCtx);
    return true;
}

} // namespace nv::infra
