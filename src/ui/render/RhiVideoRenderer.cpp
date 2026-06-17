#include "RhiVideoRenderer.h"

#include <cstdio>
#include <cstdlib>
#include <QFile>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include "src/app/ports/IFrameSurfaceRegistry.h"
#if defined(_WIN32)
#include "src/infra/video/SharedGpuDevice.h"
#endif

namespace nv::ui {

namespace {
// 풀스크린 사각형 (triangle strip): pos.xy (NDC [-1,1]) + uv. v는 위가 0 (QImage 행 순서).
const float kVertexData[] = {
    // x,    y,     u,    v
    -1.0f, -1.0f,  0.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 0.0f,
};

QShader loadShader(const QString& path) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        return QShader::fromSerialized(f.readAll());
    }
    return QShader();
}
} // namespace

RhiVideoRenderer::RhiVideoRenderer(QWidget* parent,
                                   nv::app::IFrameSurfaceRegistry* registry,
                                   std::string channelId)
    : QRhiWidget(parent), m_registry(registry), m_channelId(std::move(channelId)) {
    setMinimumSize(160, 120);
    // macOS 기본 Metal. api()는 플랫폼 기본을 사용한다(명시 setApi 불필요).
}

RhiVideoRenderer::~RhiVideoRenderer() = default;

// ── present(): UI 스레드. rhi()는 여기서 접근 불가(initialize/render 전용)이므로
//    GPU 자원 생성은 하지 않는다. GpuTexture면 CoreVideo map()만 수행(UI 스레드 안전)해
//    plane MTLTexture를 보류에 담고, render()가 createFrom으로 래핑해 그린다. ──────────
void RhiVideoRenderer::present(const nv::app::FrameSurface& surface) {
    if (surface.seq == m_seq) return;

    // GpuTexture 핸들 — 반납(releaseConsumed)할 때마다 nullptr로 만들어 이후 분기가 같은
    // 핸들을 두 번 반납(double-free)하지 않게 한다. (Windows에선 m_bridgeReady가 늘 false라
    // D3D11 경로가 반납한 뒤 macOS else-if가 또 반납하던 버그 방지.)
    void* h = (surface.kind == nv::app::FrameSurface::Kind::GpuTexture) ? surface.gpuHandle
                                                                        : nullptr;

#if defined(_WIN32)
    // 0) Windows D3D11 GPU 변환: 디코더 NV12(AVFrame 핸들)를 공유 디바이스에서 RGBA로 변환.
    //    변환이 완료되면 AVFrame은 즉시 반납(슬라이스 풀 회수). present()=render()와 같은 UI
    //    스레드라 QRhi immediate context 경합 없음.
    if (h != nullptr && m_d3dReady) {
        nv::infra::RgbaTexture rt;
        if (m_d3dBridge.convert(h, surface.width, surface.height, rt)) {
            m_d3dRgbaTex = rt.tex;
            m_d3dRgbaSize = QSize(rt.width, rt.height);
            m_hasD3dPending = true;
            m_seq = surface.seq;
            if (m_registry != nullptr) m_registry->releaseConsumed(m_channelId, h);
            update();
            return;
        }
        // 변환 실패 → 핸들 반납 후 null 처리(아래 분기 재반납 방지). RGBA 폴백은 동반 rgba가
        // 있을 때만 의미 — Windows zero-copy 프레임은 동반 rgba가 없어 그대로 마지막 프레임 유지.
        if (m_registry != nullptr) m_registry->releaseConsumed(m_channelId, h);
        h = nullptr;
    }
#endif

    // 1) NV12 zero-copy 시도 (HW 디코드 + macOS). bridge 미초기화면 건너뜀(RGBA 폴백).
    if (h != nullptr && m_bridgeReady) {
        nv::infra::PlaneTextures planes;
        if (m_bridge.map(h, planes)) {
            // 직전에 보류만 되고 아직 안 그려진 NV12 프레임이 있으면 그것부터 정리
            // (render()가 소비하기 전 새 present가 또 옴 — 스킵된 프레임). 핸들 반납.
            if (m_hasNv12Pending) {
                m_bridge.unmap(m_pendingPlanes);
                m_pendingLuma.reset();
                m_pendingChroma.reset();
                if (m_pendingHandle != nullptr && m_registry != nullptr) {
                    m_registry->releaseConsumed(m_channelId, m_pendingHandle);
                }
                m_pendingHandle = nullptr;
            }
            m_pendingPlanes = planes;
            m_pendingHandle = h;                          // 소비자 소유 ref — 우리가 인수
            m_pendingFullRange = planes.fullRange ? 1 : 0;
            m_hasNv12Pending = true;
            m_seq = surface.seq;
            update();
            return;
        }
        // map 실패 → 핸들은 우리가 받았으니 즉시 반납 후 RGBA 폴백으로.
        if (m_registry != nullptr) {
            m_registry->releaseConsumed(m_channelId, h);
        }
        h = nullptr;
    } else if (h != nullptr && m_registry != nullptr) {
        // bridge 미준비(예: 비-Apple, 캐시 실패)인데 GpuTexture 핸들을 받았다 — RGBA 폴백을
        // 쓰므로 핸들은 즉시 반납한다(누수 방지).
        m_registry->releaseConsumed(m_channelId, h);
        h = nullptr;
    }

    // 2) RGBA 폴백(CpuRgba, 또는 zero-copy 실패한 GpuTexture의 동반 rgba).
    if (surface.rgba.empty() || surface.width <= 0 || surface.height <= 0) return;
    m_seq = surface.seq;
    // 슬롯/서피스 버퍼와 수명 분리 (.copy()) — RGBA 폴백에만 발생(zero-copy는 GPU 직행).
    m_pending = QImage(surface.rgba.data(), surface.width, surface.height,
                       surface.width * 4, QImage::Format_RGBA8888)
                    .copy();
    m_hasPending = true;
    update();
}

void RhiVideoRenderer::releaseInflightNv12(bool returnHandle) {
    m_bridge.unmap(m_inflightPlanes);
    m_inflightLuma.reset();
    m_inflightChroma.reset();
    if (m_inflightHandle != nullptr) {
        if (returnHandle && m_registry != nullptr) {
            m_registry->releaseConsumed(m_channelId, m_inflightHandle);
        }
        m_inflightHandle = nullptr;
    }
}

void RhiVideoRenderer::releaseGpuResources() {
    // 보류/인플라이트 NV12 자원 — 소멸/리셋 경로에선 핸들 반납 생략(레지스트리 수명 불확실).
    if (m_hasNv12Pending) {
        m_bridge.unmap(m_pendingPlanes);
        m_pendingLuma.reset();
        m_pendingChroma.reset();
        if (m_pendingHandle != nullptr && m_registry != nullptr) {
            m_registry->releaseConsumed(m_channelId, m_pendingHandle);
        }
        m_pendingHandle = nullptr;
        m_hasNv12Pending = false;
    }
    releaseInflightNv12(/*returnHandle=*/true);

    m_nv12Pipeline.reset();
    m_nv12Srb.reset();
    m_nv12Ubuf.reset();
    m_nv12Size = QSize();

    m_pipeline.reset();
    m_srb.reset();
    m_texture.reset();
    m_texSize = QSize();

    m_sampler.reset();
    m_ubuf.reset();
    m_vbuf.reset();
    m_vbufUploaded = false;
    m_bridgeReady = false;

#if defined(_WIN32)
    // D3D11 변환 자원 해제. 디바이스 로스트 가능성 → 공유 디바이스 등록도 해제(재init이 재등록).
    m_d3dWrapTex.reset();
    m_d3dWrapSize = QSize();
    m_d3dRgbaTex = nullptr;
    m_d3dRgbaSize = QSize();
    m_hasD3dPending = false;
    if (m_d3dReady) {
        nv::infra::SharedGpuDevice::setD3d11Device(nullptr);
        m_d3dBridge.shutdown();
        m_d3dReady = false;
    }
#endif
}

void RhiVideoRenderer::releaseResources() {
    releaseGpuResources();
    m_rhi = nullptr;
}

void RhiVideoRenderer::ensureRgbaPipeline() {
    if (m_pipeline) return;
    if (!m_texture) {
        m_texture.reset(m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
        m_texture->create();
        m_texSize = QSize(1, 1);
    }
    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, m_texture.get(), m_sampler.get()),
    });
    m_srb->create();

    m_pipeline.reset(m_rhi->newGraphicsPipeline());
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/texture.vert.qsb"))},
        {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/texture.frag.qsb"))},
    });
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({{4 * sizeof(float)}});
    inputLayout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    m_pipeline->setVertexInputLayout(inputLayout);
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_pipeline->create();
}

void RhiVideoRenderer::ensureNv12Pipeline() {
    if (m_nv12Pipeline) return;

    m_nv12Ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                      sizeof(int)));
    m_nv12Ubuf->create();

    // SRB는 실제 평면 텍스처가 생긴 뒤(render) 바인딩을 갱신한다. 파이프라인 생성 시점에는
    // 1x1 placeholder 텍스처가 필요 — RGBA placeholder(m_texture)를 두 슬롯에 임시 바인딩.
    m_nv12Srb.reset(m_rhi->newShaderResourceBindings());
    m_nv12Srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, m_texture.get(), m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage, m_texture.get(), m_sampler.get()),
        QRhiShaderResourceBinding::uniformBuffer(
            3, QRhiShaderResourceBinding::FragmentStage, m_nv12Ubuf.get()),
    });
    m_nv12Srb->create();

    m_nv12Pipeline.reset(m_rhi->newGraphicsPipeline());
    m_nv12Pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_nv12Pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/texture.vert.qsb"))},
        {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/nv12.frag.qsb"))},
    });
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({{4 * sizeof(float)}});
    inputLayout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    m_nv12Pipeline->setVertexInputLayout(inputLayout);
    m_nv12Pipeline->setShaderResourceBindings(m_nv12Srb.get());
    m_nv12Pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_nv12Pipeline->create();
}

void RhiVideoRenderer::initialize(QRhiCommandBuffer*) {
    if (m_rhi != rhi()) {
        // RHI 인스턴스 교체(디바이스 로스트 등) — 모든 자원 재생성.
        releaseGpuResources();
        m_rhi = rhi();
    }

    // 공통 자원(정점/scale 유니폼/샘플러) — 1회.
    if (!m_vbuf) {
        m_vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                      sizeof(kVertexData)));
        m_vbuf->create();
        m_vbufUploaded = false;

        m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                      2 * sizeof(float)));
        m_ubuf->create();

        m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                          QRhiSampler::None, QRhiSampler::ClampToEdge,
                                          QRhiSampler::ClampToEdge));
        m_sampler->create();
    }

    ensureRgbaPipeline();   // placeholder 텍스처(m_texture)를 NV12 SRB도 공유하므로 먼저.

    // NV12 zero-copy 브리지 — QRhi가 쓰는 MTLDevice로 CVMetalTextureCache 생성(macOS만).
    // QRhiMetalNativeHandles는 Metal 백엔드(Apple)에서만 정의된다. 비-Apple은 브리지를
    // 초기화하지 않아 m_bridgeReady=false 유지 → NV12 경로 미사용, RGBA(CPU) 폴백.
#if defined(__APPLE__)
    if (!m_bridgeReady) {
        const QRhiNativeHandles* nh = m_rhi->nativeHandles();
        const auto* mh = static_cast<const QRhiMetalNativeHandles*>(nh);
        if (mh != nullptr && mh->dev != nullptr) {
            void* dev = reinterpret_cast<void*>(mh->dev);
            if (m_bridge.init(dev)) {
                m_bridgeReady = true;
                ensureNv12Pipeline();
            }
        }
    }
#endif

#if defined(_WIN32)
    // Windows D3D11 GPU 변환(zero-copy)은 기본 OFF, NV_D3D11_ZEROCOPY=1로 opt-in.
    // 이유: QRhi 디바이스를 FFmpeg와 공유하면 immediate context 1개를 UI 렌더 + 디코드
    // 스레드들이 공유 → 멀티채널에서 컨텍스트 락 경합으로 UI가 멈추고("응답없음") 죽는다.
    // 기본은 미등록 → 디코드가 자체 디바이스로 → CPU 변환 경로(안정). 하드닝 후 기본화 검토.
    if (!m_d3dReady && std::getenv("NV_D3D11_ZEROCOPY") != nullptr) {
        const QRhiNativeHandles* nh = m_rhi->nativeHandles();
        const auto* dh = static_cast<const QRhiD3D11NativeHandles*>(nh);
        if (dh != nullptr && dh->dev != nullptr) {
            if (m_d3dBridge.init(dh->dev)) {
                m_d3dReady = true;
                nv::infra::SharedGpuDevice::setD3d11Device(dh->dev);
                std::fprintf(stderr, "[RhiVideoRenderer] D3D11 zero-copy ENABLED (opt-in)\n");
            }
        }
    }
#endif
}

void RhiVideoRenderer::render(QRhiCommandBuffer* cb) {
    QRhiResourceUpdateBatch* batch = m_rhi->nextResourceUpdateBatch();

    if (!m_vbufUploaded) {
        batch->uploadStaticBuffer(m_vbuf.get(), kVertexData);
        m_vbufUploaded = true;
    }

    bool drewNew = false;
    bool useNv12 = false;

    // ── NV12 보류 프레임 소비: createFrom으로 두 평면을 QRhiTexture로 래핑 ───────────
    if (m_hasNv12Pending && m_nv12Pipeline && m_bridgeReady) {
        const int lumaW = m_pendingPlanes.width;
        const int lumaH = m_pendingPlanes.height;
        if (lumaW > 0 && lumaH > 0 &&
            m_pendingPlanes.lumaTex != nullptr && m_pendingPlanes.chromaTex != nullptr) {
            // Y = R8 (full size), CbCr = RG8 (half size).
            auto luma = std::unique_ptr<QRhiTexture>(
                m_rhi->newTexture(QRhiTexture::R8, QSize(lumaW, lumaH)));
            luma->setFormat(QRhiTexture::R8);
            luma->setPixelSize(QSize(lumaW, lumaH));
            auto chroma = std::unique_ptr<QRhiTexture>(
                m_rhi->newTexture(QRhiTexture::RG8, QSize((lumaW + 1) / 2, (lumaH + 1) / 2)));
            chroma->setFormat(QRhiTexture::RG8);
            chroma->setPixelSize(QSize((lumaW + 1) / 2, (lumaH + 1) / 2));

            QRhiTexture::NativeTexture lumaNative{
                reinterpret_cast<quint64>(m_pendingPlanes.lumaTex), 0};
            QRhiTexture::NativeTexture chromaNative{
                reinterpret_cast<quint64>(m_pendingPlanes.chromaTex), 0};

            if (luma->createFrom(lumaNative) && chroma->createFrom(chromaNative)) {
                // 직전 in-flight 자원 해제(더블버퍼) — GPU가 직전 프레임을 다 쓴 뒤.
                releaseInflightNv12(/*returnHandle=*/true);

                m_nv12Srb->setBindings({
                    QRhiShaderResourceBinding::uniformBuffer(
                        0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
                    QRhiShaderResourceBinding::sampledTexture(
                        1, QRhiShaderResourceBinding::FragmentStage, luma.get(), m_sampler.get()),
                    QRhiShaderResourceBinding::sampledTexture(
                        2, QRhiShaderResourceBinding::FragmentStage, chroma.get(), m_sampler.get()),
                    QRhiShaderResourceBinding::uniformBuffer(
                        3, QRhiShaderResourceBinding::FragmentStage, m_nv12Ubuf.get()),
                });
                m_nv12Srb->create();

                const int fr = m_pendingFullRange;
                batch->updateDynamicBuffer(m_nv12Ubuf.get(), 0, sizeof(int), &fr);

                // 보류 → in-flight 이동(소유권 이전).
                m_inflightPlanes = m_pendingPlanes;
                m_inflightLuma = std::move(luma);
                m_inflightChroma = std::move(chroma);
                m_inflightHandle = m_pendingHandle;
                m_nv12Size = QSize(lumaW, lumaH);

                m_pendingPlanes = nv::infra::PlaneTextures{};
                m_pendingHandle = nullptr;
                m_hasNv12Pending = false;
                useNv12 = true;
                drewNew = true;

                if (!m_loggedPath) {
                    std::fprintf(stderr, "[RhiVideoRenderer] render path = NV12 zero-copy\n");
                    m_loggedPath = true;
                }
            } else {
                // createFrom 실패 — 보류 NV12 폐기 후 RGBA 폴백.
                m_bridge.unmap(m_pendingPlanes);
                if (m_pendingHandle != nullptr && m_registry != nullptr) {
                    m_registry->releaseConsumed(m_channelId, m_pendingHandle);
                }
                m_pendingPlanes = nv::infra::PlaneTextures{};
                m_pendingHandle = nullptr;
                m_hasNv12Pending = false;
            }
        } else {
            // 유효치 않은 보류 — 폐기.
            m_bridge.unmap(m_pendingPlanes);
            if (m_pendingHandle != nullptr && m_registry != nullptr) {
                m_registry->releaseConsumed(m_channelId, m_pendingHandle);
            }
            m_pendingPlanes = nv::infra::PlaneTextures{};
            m_pendingHandle = nullptr;
            m_hasNv12Pending = false;
        }
    }

#if defined(_WIN32)
    // ── Windows D3D11 변환 결과(RGBA) 소비: createFrom으로 RHI 텍스처 래핑 후 RGBA 파이프라인 ──
    if (m_hasD3dPending && m_d3dReady && m_d3dRgbaTex != nullptr) {
        const QSize sz = m_d3dRgbaSize;
        if (sz.width() > 0 && sz.height() > 0) {
            // 브리지 RGBA tex는 같은 크기면 포인터가 안정적(재사용) — 크기 변할 때만 재래핑.
            // 같은 크기 후속 프레임은 convert가 같은 tex 내용을 갱신하므로 재바인딩 불필요.
            if (!m_d3dWrapTex || m_d3dWrapSize != sz) {
                m_d3dWrapTex.reset(m_rhi->newTexture(QRhiTexture::RGBA8, sz));
                m_d3dWrapTex->setFormat(QRhiTexture::RGBA8);
                m_d3dWrapTex->setPixelSize(sz);
                QRhiTexture::NativeTexture nt{reinterpret_cast<quint64>(m_d3dRgbaTex), 0};
                if (m_d3dWrapTex->createFrom(nt)) {
                    m_d3dWrapSize = sz;
                    m_srb->setBindings({
                        QRhiShaderResourceBinding::uniformBuffer(
                            0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
                        QRhiShaderResourceBinding::sampledTexture(
                            1, QRhiShaderResourceBinding::FragmentStage,
                            m_d3dWrapTex.get(), m_sampler.get()),
                    });
                    m_srb->create();
                } else {
                    m_d3dWrapTex.reset();
                }
            }
            if (m_d3dWrapTex) {
                m_texSize = sz;       // 아스펙트 계산 + RGBA 파이프라인 선택용
                drewNew = true;
                if (!m_loggedPath) {
                    std::fprintf(stderr, "[RhiVideoRenderer] render path = D3D11 GPU-convert\n");
                    m_loggedPath = true;
                }
            }
        }
        m_hasD3dPending = false;
    }
#endif

    // ── RGBA 폴백 텍스처 업로드 (NV12를 안 쓸 때만) ──────────────────────────────
    if (!useNv12 && m_hasPending && !m_pending.isNull()) {
        const QSize imgSize = m_pending.size();
        if (imgSize != m_texSize) {
            m_texture.reset(m_rhi->newTexture(QRhiTexture::RGBA8, imgSize));
            m_texture->create();
            m_texSize = imgSize;
            m_srb->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(
                    0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
                QRhiShaderResourceBinding::sampledTexture(
                    1, QRhiShaderResourceBinding::FragmentStage, m_texture.get(),
                    m_sampler.get()),
            });
            m_srb->create();
        }
        batch->uploadTexture(m_texture.get(), m_pending);
        m_hasPending = false;
        drewNew = true;
        // RGBA로 전환 — 이전 NV12 in-flight 핸들 반납(스테일 보관 방지).
        releaseInflightNv12(/*returnHandle=*/true);
        if (!m_loggedPath) {
            std::fprintf(stderr, "[RhiVideoRenderer] render path = RGBA\n");
            m_loggedPath = true;
        }
    }

    // KeepAspectRatio 레터박스: 그릴 텍스처의 픽셀 크기로 NDC 배율 계산.
    const QSize out = renderTarget()->pixelSize();
    const QSize contentSize = useNv12 ? m_nv12Size : m_texSize;
    float scaleX = 1.0f, scaleY = 1.0f;
    if (contentSize.width() > 0 && contentSize.height() > 0 &&
        out.width() > 0 && out.height() > 0) {
        const float texAR = static_cast<float>(contentSize.width()) / contentSize.height();
        const float winAR = static_cast<float>(out.width()) / out.height();
        if (winAR > texAR) {
            scaleX = texAR / winAR;
        } else {
            scaleY = winAR / texAR;
        }
    }
    const float scaleData[2] = {scaleX, scaleY};
    batch->updateDynamicBuffer(m_ubuf.get(), 0, 2 * sizeof(float), scaleData);

    // 그릴 파이프라인 선택: NV12 in-flight 자원이 있으면 NV12(이번에 새로 만들었든, 직전
    // 프레임을 다시 그리는 repaint이든). RGBA 폴백 텍스처를 새로 올렸으면(useNv12==false &&
    // drewNew via RGBA) RGBA. 둘 다 없으면 마지막 상태 유지(in-flight 우선).
    const bool drawNv12 = m_nv12Pipeline && m_inflightLuma && (useNv12 || !drewNew);
    QRhiGraphicsPipeline* pipe = nullptr;
    QRhiShaderResourceBindings* srb = nullptr;
    if (drawNv12) {
        pipe = m_nv12Pipeline.get();
        srb = m_nv12Srb.get();
    } else {
        pipe = m_pipeline.get();
        srb = m_srb.get();
    }

    cb->beginPass(renderTarget(), QColor(Qt::black), {1.0f, 0}, batch);
    cb->setGraphicsPipeline(pipe);
    cb->setViewport(QRhiViewport(0, 0, out.width(), out.height()));
    cb->setShaderResources(srb);
    const QRhiCommandBuffer::VertexInput vbufBinding(m_vbuf.get(), 0);
    cb->setVertexInput(0, 1, &vbufBinding);
    cb->draw(4);
    cb->endPass();

    if (drewNew) {
        emit framePainted();   // 표시 확정 — 새 프레임을 GPU에 제출했을 때만
    }
}

} // namespace nv::ui
