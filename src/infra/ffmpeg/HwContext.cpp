#include "HwContext.h"
#include <cstdio>
#include <cstdlib>

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
    // NV_FORCE_SW=1 이면 HW 디코딩을 강제 비활성화 — 성능 비교·문제 진단용 영구 스위치.
    // 기본(미설정) 실행에는 영향 없음.
    if (std::getenv("NV_FORCE_SW") != nullptr) {
        std::fprintf(stderr, "[HwContext] SW forced (NV_FORCE_SW)\n");
        return false;
    }
    if (kHwType == AV_HWDEVICE_TYPE_NONE) {
        std::fprintf(stderr, "[HwContext] HW unsupported on this platform\n");
        return false;
    }

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
    if (hwPix == AV_PIX_FMT_NONE) {
        std::fprintf(stderr, "[HwContext] codec has no HW pixfmt for this device\n");
        return false;   // 이 코덱+디바이스 조합 미지원
    }

    // 2) hw 디바이스 컨텍스트 생성.
    AVBufferRef* devCtx = nullptr;
    const int createRc = av_hwdevice_ctx_create(&devCtx, kHwType, nullptr, nullptr, 0);
    if (createRc < 0) {
        std::fprintf(stderr, "[HwContext] hwdevice create failed (rc=%d)\n", createRc);
        return false;   // 디바이스 없음/생성 실패 → SW 폴백
    }

    // 3) 디코더에 배선: get_format 콜백 + hw_device_ctx ref + opaque(self).
    m_deviceCtx = devCtx;
    m_hwPixFmt = hwPix;
    dec->opaque = this;
    dec->get_format = &HwContext::getFormat;
    dec->hw_device_ctx = av_buffer_ref(m_deviceCtx);
    if (dec->hw_device_ctx == nullptr) {
        std::fprintf(stderr, "[HwContext] av_buffer_ref failed — falling back to SW\n");
        return false;   // SW 폴백 (devCtx는 멤버이므로 소멸 시 정리)
    }
    return true;
}

// 플랫폼별 GPU 네이티브 핸들 추출 (R3: data[3] 직접 사용 대신 이 함수 경유).
void* HwContext::extractGpuHandle(const AVFrame* frame) {
#if defined(__APPLE__)
    return frame->data[3];   // VideoToolbox CVPixelBufferRef
#elif defined(_WIN32)
    return frame->data[0];   // D3D11 ID3D11Texture2D* (data[1]은 텍스처 배열 인덱스)
#else
    (void)frame;
    return nullptr;
#endif
}

} // namespace nv::infra
