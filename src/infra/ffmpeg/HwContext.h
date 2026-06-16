#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace nv::infra {

// 플랫폼별 하드웨어 디코딩 컨텍스트 (설계 M2b Task4).
// macOS: VideoToolbox(AV_HWDEVICE_TYPE_VIDEOTOOLBOX / AV_PIX_FMT_VIDEOTOOLBOX).
// Windows: D3D11VA — 이 태스크는 컴파일 스텁만 두고 실제 활성은 Task6에서.
//
// 사용 순서(정본 doc/examples/hw_decode.c 따름):
//   1) HwContext hw; if (hw.init(dec, codec)) { ... hw 경로 ... }
//      init()은 avcodec_get_hw_config로 hw_pix_fmt를 찾고
//      av_hwdevice_ctx_create로 디바이스를 만들어 dec->hw_device_ctx에 ref,
//      dec->opaque=this, dec->get_format=&HwContext::getFormat 를 설정한다.
//   2) avcodec_open2(dec, ...) 호출은 호출측 책임 (init 이후).
//   3) 디코드 루프에서 frame->format == hwPixFmt() 이면 hw 프레임.
//
// init()이 false면 hw 미지원/실패 — 호출측은 기존 SW 경로로 폴백한다.
// 이 클래스는 디코드 스레드에서만 쓰인다(스레드 안전 보장 안 함).
class HwContext {
public:
    HwContext() = default;
    ~HwContext();

    HwContext(const HwContext&) = delete;
    HwContext& operator=(const HwContext&) = delete;

    // dec/codec에 대해 hw 디바이스를 만들고 get_format 콜백을 배선한다.
    // 성공 시 true (hw 경로 가능), 실패/미지원 시 false (SW 폴백).
    // avcodec_open2() 호출 전에 불러야 한다.
    bool init(AVCodecContext* dec, const AVCodec* codec);

    // 협상된 hw 픽셀 포맷 (init 성공 후 유효). 미초기화면 AV_PIX_FMT_NONE.
    AVPixelFormat hwPixFmt() const { return m_hwPixFmt; }

    bool active() const { return m_deviceCtx != nullptr; }

    // 플랫폼별 GPU 네이티브 핸들 추출.
    // macOS: data[3] = CVPixelBufferRef (VideoToolbox)
    // Windows: data[0] = ID3D11Texture2D* (data[1]은 텍스처 배열 인덱스)
    // 그 외: nullptr
    static void* extractGpuHandle(const AVFrame* frame);

    // D3D11 텍스처 배열 슬라이스 인덱스(AVFrame.data[1]). 그 외 플랫폼은 0.
    static int extractGpuIndex(const AVFrame* frame);

    // Windows에서 SharedGpuDevice 공유 디바이스로 hw 컨텍스트를 만들었으면 true.
    // (디코더 텍스처가 렌더 디바이스와 같아 GPU 변환 zero-copy 가능 — 아니면 CPU 폴백.)
    bool sharesRenderDevice() const { return m_sharedDevice; }

private:
    // get_format 콜백: hw_pix_fmt가 후보에 있으면 그것을, 없으면 첫 SW 포맷 폴백.
    static AVPixelFormat getFormat(AVCodecContext* ctx, const AVPixelFormat* fmts);

    AVBufferRef* m_deviceCtx = nullptr;
    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE;
    bool m_sharedDevice = false;   // Windows: QRhi 공유 디바이스로 생성됨(zero-copy 가능)
};

} // namespace nv::infra
