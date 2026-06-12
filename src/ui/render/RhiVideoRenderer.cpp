#include "RhiVideoRenderer.h"

#include <QFile>
#include <rhi/qrhi.h>

namespace nv::ui {

namespace {
// 풀스크린 사각형 (triangle strip): pos.xy (NDC [-1,1]) + uv. v는 위가 0 (QImage 행 순서).
// QRhi NDC는 백엔드별로 다르나 QRhiWidget이 clipSpaceCorrMatrix를 쓰지 않는 단순 풀스크린에서는
// y 반전을 mirrorVertically 또는 uv로 처리. 여기서는 uv.v를 위=0으로 두고 텍스처를 그대로 그린다.
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

RhiVideoRenderer::RhiVideoRenderer(QWidget* parent) : QRhiWidget(parent) {
    setMinimumSize(160, 120);
    // macOS 기본 Metal. api()는 플랫폼 기본을 사용한다(명시 setApi 불필요).
}

RhiVideoRenderer::~RhiVideoRenderer() = default;

void RhiVideoRenderer::present(const nv::app::FrameSurface& surface) {
    if (surface.seq == m_seq) return;
    if (surface.rgba.empty() || surface.width <= 0 || surface.height <= 0) return;
    m_seq = surface.seq;
    // 슬롯/서피스 버퍼와 수명 분리 (.copy()).
    m_pending = QImage(surface.rgba.data(), surface.width, surface.height,
                       surface.width * 4, QImage::Format_RGBA8888)
                    .copy();
    m_hasPending = true;
    update();   // QRhiWidget render() 1회 유발
}

void RhiVideoRenderer::releaseGpuResources() {
    m_pipeline.reset();
    m_srb.reset();
    m_sampler.reset();
    m_texture.reset();
    m_ubuf.reset();
    m_vbuf.reset();
    m_texSize = QSize();
    m_vbufUploaded = false;
}

void RhiVideoRenderer::releaseResources() {
    releaseGpuResources();
    m_rhi = nullptr;
}

void RhiVideoRenderer::initialize(QRhiCommandBuffer*) {
    if (m_rhi != rhi()) {
        // RHI 인스턴스 교체(디바이스 로스트 등) — 모든 자원 재생성.
        releaseGpuResources();
        m_rhi = rhi();
    }

    if (!m_pipeline) {
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

        // 텍스처는 첫 프레임 크기에 맞춰 render()에서 생성. 여기서는 1x1 placeholder로
        // SRB/파이프라인을 구성해 두고, 크기 변경 시 텍스처만 교체한다.
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
}

void RhiVideoRenderer::render(QRhiCommandBuffer* cb) {
    QRhiResourceUpdateBatch* batch = m_rhi->nextResourceUpdateBatch();

    if (!m_vbufUploaded) {
        batch->uploadStaticBuffer(m_vbuf.get(), kVertexData);
        m_vbufUploaded = true;
    }

    bool drewNew = false;
    if (m_hasPending && !m_pending.isNull()) {
        const QSize imgSize = m_pending.size();
        if (imgSize != m_texSize) {
            // 해상도 변경 — 텍스처 재생성 후 SRB 갱신.
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
        // QImage(RGBA8888) → 텍스처 업로드. QRhi는 RGBA8을 기대.
        batch->uploadTexture(m_texture.get(), m_pending);
        m_hasPending = false;
        drewNew = true;
    }

    // KeepAspectRatio 레터박스: 텍스처 종횡비를 위젯 종횡비에 맞춰 NDC 배율 계산.
    const QSize out = renderTarget()->pixelSize();
    float scaleX = 1.0f, scaleY = 1.0f;
    if (m_texSize.width() > 0 && m_texSize.height() > 0 && out.width() > 0 && out.height() > 0) {
        const float texAR = static_cast<float>(m_texSize.width()) / m_texSize.height();
        const float winAR = static_cast<float>(out.width()) / out.height();
        if (winAR > texAR) {
            scaleX = texAR / winAR;   // 창이 더 넓음 — 좌우 레터박스
        } else {
            scaleY = winAR / texAR;   // 창이 더 높음 — 상하 레터박스
        }
    }
    const float scaleData[2] = {scaleX, scaleY};
    batch->updateDynamicBuffer(m_ubuf.get(), 0, 2 * sizeof(float), scaleData);

    cb->beginPass(renderTarget(), QColor(Qt::black), {1.0f, 0}, batch);
    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setViewport(QRhiViewport(0, 0, out.width(), out.height()));
    cb->setShaderResources();
    const QRhiCommandBuffer::VertexInput vbufBinding(m_vbuf.get(), 0);
    cb->setVertexInput(0, 1, &vbufBinding);
    cb->draw(4);   // triangle strip 4 verts
    cb->endPass();

    if (drewNew) {
        emit framePainted();   // 표시 확정 — 새 프레임을 GPU에 제출했을 때만
    }
    // 연속 update() 호출하지 않음 — present()가 새 프레임마다 update()를 호출(폴링 구동).
}

} // namespace nv::ui
