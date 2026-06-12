#include "VideoTileWidget.h"
#include <atomic>
#include <cstdio>
#include <QVBoxLayout>
#include "src/app/ports/IVideoRenderer.h"
#include "src/ui/render/RhiVideoRenderer.h"
#include "src/ui/render/SwVideoRenderer.h"

namespace nv::ui {

// ── 렌더러 RHI 프로브 1회 캐시 (#20) ────────────────────────────────────────────
// 문제: QRhi 불가 환경에서 20채널이 각자 RhiVideoRenderer 생성 → renderFailed →
//      fallbackToSw() 순으로 실패하면 20번의 RHI 초기화 시도·폴백이 연속 발생해
//      화면 글리치 폭풍이 생긴다.
// 해결: 프로세스 수명 동안 QRhi 가용성 결과를 정적 캐시에 기록한다.
//      첫 타일이 renderFailed를 받으면 s_rhiProbeState를 2(불가)로 설정하고,
//      이후 생성되는 모든 타일은 RHI 시도 없이 바로 SW 렌더러로 시작한다.
//
//  0 = 미탐지(최초 또는 성공 이후) — RHI 시도
//  1 = 가용 확인 — RHI 시도 (미래 확장용, 현재는 0과 동일 동작)
//  2 = 불가 확인 — 즉시 SW 선택
//
// 이 캐시는 단방향(불가→가용 복귀 없음)이며 프로세스 재시작 시 초기화된다.
// (GPU가 런타임에 다시 생기는 시나리오는 실용적으로 무시: 재시작이 더 안전.)
static std::atomic<int> s_rhiProbeState{0};  // 0=unprobed, 1=ok, 2=unavailable

VideoTileWidget::VideoTileWidget(nv::app::IFrameSurfaceRegistry& registry,
                                 std::string channelId,
                                 QWidget* parent)
    : QWidget(parent), m_registry(registry), m_channelId(std::move(channelId)) {
    setMinimumSize(320, 240);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    // RHI 가용성 1회 캐시 프로브: 이미 불가로 판정된 환경이면 바로 SW 선택.
    // 미탐지·가용 환경은 RHI 시도 → renderFailed 시 fallbackToSw()가 캐시를 갱신.
    const bool rhiOk = (s_rhiProbeState.load(std::memory_order_acquire) != 2);
    installRenderer(selectRendererKind(rhiOk));
}

void VideoTileWidget::installRenderer(RendererKind kind) {
    m_kind = kind;
    if (kind == RendererKind::Rhi) {
        // RHI 렌더러는 GpuTexture(NV12 zero-copy) 핸들의 수명을 직접 소유한다 — registry/
        // channelId를 주입해 in-flight 보관 후 releaseConsumed로 반납하게 한다(C3 계약).
        auto* rhi = new RhiVideoRenderer(this, &m_registry, m_channelId);
        m_renderer = rhi;
        connect(rhi, &RhiVideoRenderer::framePainted, this, &VideoTileWidget::framePainted);
        // QRhi 초기화/렌더 실패 시 런타임 SW 폴백.
        connect(rhi, &QRhiWidget::renderFailed, this, &VideoTileWidget::fallbackToSw);
    } else {
        auto* sw = new SwVideoRenderer(this);
        m_renderer = sw;
        connect(sw, &SwVideoRenderer::framePainted, this, &VideoTileWidget::framePainted);
    }
    m_layout->addWidget(m_renderer->widget());
}

void VideoTileWidget::fallbackToSw() {
    if (m_fellBack || m_kind == RendererKind::Sw) return;
    // GPU 렌더 경로 실패 — SW(QPainter)로 런타임 전환(영상은 계속 표시). 운영 가시성 1회 로그.
    std::fprintf(stderr, "[VideoTileWidget] render path = SW (QRhi fallback)\n");
    m_fellBack = true;
    // RHI 불가로 판정 — 이후 생성되는 타일은 바로 SW로 시작해 글리치 폭풍을 막는다 (#20).
    s_rhiProbeState.store(2, std::memory_order_release);
    // 기존 RHI 위젯 제거 후 SW 렌더러로 교체.
    QWidget* old = m_renderer->widget();
    m_layout->removeWidget(old);
    old->deleteLater();
    m_renderer = nullptr;
    m_seq = 0;   // 다음 폴링에서 최신 프레임을 SW로 다시 그리도록
    installRenderer(RendererKind::Sw);
}

void VideoTileWidget::pollFrame() {
    if (m_renderer == nullptr) return;

    // D9 오프스크린 폴링 스킵: 화면에 보이지 않는 타일은 latestSurface 조회·RGBA 복사를
    // 건너뛴다. latestSurface는 슬롯 RGBA(최대 8MB/프레임)를 깊은 복사하므로, 20채널 중
    // 스크롤 밖·숨김 타일까지 매 펄스(30Hz) 복사하면 불필요한 메모리 대역폭이 든다.
    //   • isVisible()==false: 다른 그리드 페이지로 hide()된 타일.
    //   • visibleRegion().isEmpty(): 스크롤 영역 안에서 뷰포트 밖으로 스크롤된 타일.
    // 보이는 타일만 폴링해 무용한 복사를 제거한다. 다시 보이면 m_seq 가드로 최신 프레임을
    // 즉시 받아 그린다(상태 손실 없음). (zero-copy GPU 경로여도 동반 RGBA 복사가 슬롯
    // latest()에서 일어나므로 효과가 있다.)
    if (!isVisible() || visibleRegion().isEmpty()) return;

    // IFrameSurfaceRegistry 포트로 최신 서피스 조회 (CpuRgba / GpuTexture 모두).
    nv::app::FrameSurface surface;
    if (!m_registry.latestSurface(m_channelId, surface, m_seq)) return;
    m_seq = surface.seq;

    m_renderer->present(surface);

    // GpuTexture 핸들 수명(C3 계약):
    //  - RHI 렌더러: 핸들을 in-flight 보관 후 스스로 releaseConsumed로 반납한다 → 여기서 안 함.
    //  - SW 렌더러: gpuHandle을 무시(동반 RGBA만 그림)하므로 즉시 반납해야 누수가 없다.
    if (m_kind == RendererKind::Sw &&
        surface.kind == nv::app::FrameSurface::Kind::GpuTexture &&
        surface.gpuHandle != nullptr) {
        m_registry.releaseConsumed(m_channelId, surface.gpuHandle);
    }
}

} // namespace nv::ui
