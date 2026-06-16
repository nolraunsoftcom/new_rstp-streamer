#include "HwContext.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#if defined(_WIN32)
// d3d11.h를 extern "C" 밖에서 먼저 include한다 — d3d11.h는 D3D11_VIEWPORT/RECT/BOX 등에
// C++ operator==/!= 오버로드를 정의하는데, hwcontext_d3d11va.h가 내부적으로 d3d11.h를
// include하므로 그 안(extern "C")에서 처음 포함되면 연산자가 C 링키지로 묶여 C2733이 난다.
// 먼저 C++ 링키지로 포함해 두면 include 가드가 재포함을 막아 문제를 피한다.
#include <d3d11.h>
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#include "src/infra/video/SharedGpuDevice.h"
#endif

namespace nv::infra {

namespace {
// 이 빌드가 타깃하는 hw 디바이스 타입과 hw 픽셀 포맷.
// macOS는 VideoToolbox. Windows는 D3D11VA.
#if defined(__APPLE__)
constexpr AVHWDeviceType kHwType = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(_WIN32)
constexpr AVHWDeviceType kHwType = AV_HWDEVICE_TYPE_D3D11VA;
#else
constexpr AVHWDeviceType kHwType = AV_HWDEVICE_TYPE_NONE;
#endif

#if defined(_WIN32)
// D3D11VA frames context에 SHADER_RESOURCE 바인드를 추가한다 — 디코더 출력 텍스처를 SRV로
// 읽어 GPU 변환(NV12→RGBA)하기 위해 필수. 기본(BIND_DECODER만)이면 CreateShaderResourceView가
// 거부된다. 실패 시 FFmpeg 자동 frames로 두면 브리지 convert가 실패해 CPU 폴백된다(무해).
void setupD3d11FramesCtx(AVCodecContext* ctx, AVPixelFormat hwPix) {
    AVBufferRef* frames = nullptr;
    if (avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx, hwPix, &frames) < 0 ||
        frames == nullptr) {
        return;
    }
    auto* fctx  = reinterpret_cast<AVHWFramesContext*>(frames->data);
    auto* d3dfc = reinterpret_cast<AVD3D11VAFramesContext*>(fctx->hwctx);
    d3dfc->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (av_hwframe_ctx_init(frames) < 0) {
        av_buffer_unref(&frames);
        return;
    }
    av_buffer_unref(&ctx->hw_frames_ctx);
    ctx->hw_frames_ctx = frames;   // 소유권 이전(디코더가 이 frames pool 사용)
}
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
        if (*p == hw) {   // hw surface 포맷 선택
#if defined(_WIN32)
            // 공유 디바이스로 만든 경우에만 SHADER_RESOURCE frames를 준비(zero-copy 경로).
            // 자체 디바이스면 어차피 크로스 디바이스라 CPU 폴백 → frames 커스터마이즈 불필요.
            if (self != nullptr && self->m_sharedDevice) {
                setupD3d11FramesCtx(ctx, hw);
            }
#endif
            return *p;
        }
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
#if defined(_WIN32)
    // Windows: 가능하면 QRhi 공유 ID3D11Device로 만든다(디코더 텍스처 = 렌더 디바이스 →
    // GPU 변환 zero-copy). 미등록/실패 시 자체 디바이스로 폴백(크로스 디바이스 → CPU 변환).
    if (void* shared = SharedGpuDevice::d3d11Device()) {
        devCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (devCtx != nullptr) {
            auto* hwdc   = reinterpret_cast<AVHWDeviceContext*>(devCtx->data);
            auto* d3dctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwdc->hwctx);
            d3dctx->device = static_cast<ID3D11Device*>(shared);
            d3dctx->device->AddRef();   // FFmpeg가 디바이스 free 시 Release
            if (av_hwdevice_ctx_init(devCtx) < 0) {
                av_buffer_unref(&devCtx);   // 공유 init 실패 → 자체 디바이스 폴백
                devCtx = nullptr;
            } else {
                m_sharedDevice = true;
                std::fprintf(stderr, "[HwContext] D3D11VA on shared QRhi device (zero-copy)\n");
            }
        }
    }
    if (devCtx == nullptr) {
        const int createRc = av_hwdevice_ctx_create(&devCtx, kHwType, nullptr, nullptr, 0);
        if (createRc < 0) {
            std::fprintf(stderr, "[HwContext] hwdevice create failed (rc=%d)\n", createRc);
            return false;
        }
    }
#else
    const int createRc = av_hwdevice_ctx_create(&devCtx, kHwType, nullptr, nullptr, 0);
    if (createRc < 0) {
        std::fprintf(stderr, "[HwContext] hwdevice create failed (rc=%d)\n", createRc);
        return false;   // 디바이스 없음/생성 실패 → SW 폴백
    }
#endif

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

int HwContext::extractGpuIndex(const AVFrame* frame) {
#if defined(_WIN32)
    return static_cast<int>(reinterpret_cast<intptr_t>(frame->data[1]));
#else
    (void)frame;
    return 0;
#endif
}

} // namespace nv::infra
