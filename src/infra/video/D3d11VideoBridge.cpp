#include "src/infra/video/D3d11VideoBridge.h"

#if defined(_WIN32)

#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
}

namespace nv::infra {

namespace {

// 풀스크린 삼각형(정점버퍼 없이 SV_VertexID로) + NV12→RGBA 변환 픽셀 셰이더.
// 디코더 텍스처는 Texture2DArray(NV12)이고 frames ctx에 SHADER_RESOURCE를 추가해(HwContext)
// Y=R8_UNORM, UV=R8G8_UNORM SRV(배열 슬라이스 지정)로 평면을 읽는다. BT.709 video-range 변환.
const char* kVertexHlsl = R"(
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut main(uint vid : SV_VertexID) {
    VOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

const char* kPixelHlsl = R"(
Texture2DArray<float>  YPlane  : register(t0);
Texture2DArray<float2> UVPlane : register(t1);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float  y  = YPlane.Sample(samp, float3(uv, 0.0)).r;
    float2 uvc = UVPlane.Sample(samp, float3(uv, 0.0)).rg;
    // limited-range(video) → full-range 정규화
    y = (y - 0.0627451) * 1.164384;          // (y - 16/255) * 255/219
    float u = (uvc.x - 0.5) * 1.138393;      // 255/224
    float v = (uvc.y - 0.5) * 1.138393;
    float r = y + 1.5748 * v;
    float g = y - 0.187324 * u - 0.468124 * v;
    float b = y + 1.8556 * u;
    return float4(saturate(float3(r, g, b)), 1.0);
}
)";

ID3DBlob* compile(const char* src, const char* entry, const char* target) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                                  entry, target, 0, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err != nullptr) {
            std::fprintf(stderr, "[D3d11VideoBridge] shader compile fail: %s\n",
                         static_cast<const char*>(err->GetBufferPointer()));
            err->Release();
        }
        return nullptr;
    }
    if (err != nullptr) err->Release();
    return blob;
}

template <class T> void safeRelease(T*& p) { if (p != nullptr) { p->Release(); p = nullptr; } }

} // namespace

struct D3d11VideoBridge::Impl {
    ID3D11Device*           dev  = nullptr;
    ID3D11DeviceContext*    ctx  = nullptr;
    ID3D11VertexShader*     vs   = nullptr;
    ID3D11PixelShader*      ps   = nullptr;
    ID3D11SamplerState*     samp = nullptr;
    ID3D11Texture2D*        rgba = nullptr;   // 변환 결과(재사용, 크기 변하면 재생성)
    ID3D11RenderTargetView* rtv  = nullptr;
    int rgbaW = 0;
    int rgbaH = 0;

    ~Impl() {
        safeRelease(rtv);
        safeRelease(rgba);
        safeRelease(samp);
        safeRelease(ps);
        safeRelease(vs);
        safeRelease(ctx);
        safeRelease(dev);
    }

    bool ensureRgba(int w, int h) {
        if (rgba != nullptr && rgbaW == w && rgbaH == h) return true;
        safeRelease(rtv);
        safeRelease(rgba);
        rgbaW = 0; rgbaH = 0;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &rgba))) return false;
        if (FAILED(dev->CreateRenderTargetView(rgba, nullptr, &rtv))) {
            safeRelease(rgba);
            return false;
        }
        rgbaW = w; rgbaH = h;
        return true;
    }
};

D3d11VideoBridge::~D3d11VideoBridge() { shutdown(); }

bool D3d11VideoBridge::init(void* d3d11Device) {
    if (m_impl != nullptr) return true;
    if (d3d11Device == nullptr) return false;

    auto* impl = new Impl();
    impl->dev = static_cast<ID3D11Device*>(d3d11Device);
    impl->dev->AddRef();
    impl->dev->GetImmediateContext(&impl->ctx);

    ID3DBlob* vsb = compile(kVertexHlsl, "main", "vs_5_0");
    ID3DBlob* psb = compile(kPixelHlsl, "main", "ps_5_0");
    bool ok = (vsb != nullptr && psb != nullptr);
    if (ok) {
        ok = SUCCEEDED(impl->dev->CreateVertexShader(
                 vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &impl->vs)) &&
             SUCCEEDED(impl->dev->CreatePixelShader(
                 psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &impl->ps));
    }
    if (vsb != nullptr) vsb->Release();
    if (psb != nullptr) psb->Release();

    if (ok) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(impl->dev->CreateSamplerState(&sd, &impl->samp));
    }

    if (!ok) {
        delete impl;
        std::fprintf(stderr, "[D3d11VideoBridge] init failed\n");
        return false;
    }
    m_impl = impl;
    std::fprintf(stderr, "[D3d11VideoBridge] init ok (GPU NV12->RGBA convert)\n");
    return true;
}

bool D3d11VideoBridge::convert(void* gpuFrame, int width, int height, RgbaTexture& out) {
    if (m_impl == nullptr || gpuFrame == nullptr || width <= 0 || height <= 0) return false;
    Impl* d = m_impl;

    // AVFrame에서 디코더 텍스처(data[0])와 배열 슬라이스(data[1])를 추출.
    auto* frame = static_cast<AVFrame*>(gpuFrame);
    auto* tex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const int arrayIndex = static_cast<int>(reinterpret_cast<intptr_t>(frame->data[1]));
    if (tex == nullptr) return false;

    if (!d->ensureRgba(width, height)) return false;

    // Y(R8) / UV(R8G8) SRV — 디코더 텍스처 배열의 arrayIndex 슬라이스. (frames ctx에
    // SHADER_RESOURCE 바인드가 있어야 성공 — 없으면 false 반환 → 호출측 CPU 폴백.)
    ID3D11ShaderResourceView* ySrv = nullptr;
    ID3D11ShaderResourceView* uvSrv = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MostDetailedMip = 0;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.FirstArraySlice = static_cast<UINT>(arrayIndex);
    sd.Texture2DArray.ArraySize = 1;

    // NV12 평면 선택은 SRV 포맷으로 한다(D3D11은 PlaneSlice 미사용 — D3D12 개념).
    // R8_UNORM=Y(plane0, full res), R8G8_UNORM=UV(plane1, half res).
    sd.Format = DXGI_FORMAT_R8_UNORM;
    HRESULT hr = d->dev->CreateShaderResourceView(tex, &sd, &ySrv);
    if (SUCCEEDED(hr)) {
        sd.Format = DXGI_FORMAT_R8G8_UNORM;
        hr = d->dev->CreateShaderResourceView(tex, &sd, &uvSrv);
    }
    if (FAILED(hr)) {
        safeRelease(ySrv);
        return false;
    }

    // 변환 패스: RGBA RTV로 풀스크린 삼각형 그리기. (immediate context 상태는 RHI가 다음
    // beginPass에서 전부 재바인드하므로 복원 불필요 — present()=render()와 같은 UI 스레드.)
    ID3D11DeviceContext* c = d->ctx;
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    c->OMSetRenderTargets(1, &d->rtv, nullptr);
    c->RSSetViewports(1, &vp);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(d->vs, nullptr, 0);
    c->PSSetShader(d->ps, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {ySrv, uvSrv};
    c->PSSetShaderResources(0, 2, srvs);
    c->PSSetSamplers(0, 1, &d->samp);
    c->Draw(3, 0);

    // SRV 언바인드(다음 프레임 디코더 텍스처 재사용 시 read/write 해저드 경고 방지) + 해제.
    ID3D11ShaderResourceView* nullSrvs[2] = {nullptr, nullptr};
    c->PSSetShaderResources(0, 2, nullSrvs);
    ID3D11RenderTargetView* nullRtv = nullptr;
    c->OMSetRenderTargets(1, &nullRtv, nullptr);
    safeRelease(uvSrv);
    safeRelease(ySrv);

    out.tex = d->rgba;
    out.width = width;
    out.height = height;
    return true;
}

void D3d11VideoBridge::shutdown() {
    delete m_impl;
    m_impl = nullptr;
}

} // namespace nv::infra

#else  // ── 비-Windows 스텁 (macOS/Linux는 D3D11 미존재) ──────────────────────────

namespace nv::infra {

struct D3d11VideoBridge::Impl {};
D3d11VideoBridge::~D3d11VideoBridge() = default;
bool D3d11VideoBridge::init(void*) { return false; }
bool D3d11VideoBridge::convert(void*, int, int, RgbaTexture&) { return false; }
void D3d11VideoBridge::shutdown() {}

} // namespace nv::infra

#endif // _WIN32
