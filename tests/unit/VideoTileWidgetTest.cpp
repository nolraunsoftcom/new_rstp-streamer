#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <string>
#include <vector>
#include "src/app/ports/IFrameSurfaceRegistry.h"
#include "src/ui/grid/VideoTileWidget.h"
#include "src/ui/render/RhiVideoRenderer.h"

// ── Fake IFrameSurfaceRegistry ──────────────────────────────────────────────
namespace {

struct FakeRegistry : public nv::app::IFrameSurfaceRegistry {
    // 호출 시 m_surface를 채우고 true를 반환. seq가 lastSeq와 같으면 false.
    nv::app::FrameSurface m_surface;
    bool m_hasNew = false;

    int releaseConsumedCallCount = 0;
    void* lastReleasedHandle = nullptr;

    bool latestSurface(const std::string& /*channelId*/,
                       nv::app::FrameSurface& out,
                       uint64_t lastSeq) override {
        if (!m_hasNew || m_surface.seq == lastSeq) return false;
        out = m_surface;
        return true;
    }

    void releaseConsumed(const std::string& /*channelId*/, void* handle) override {
        ++releaseConsumedCallCount;
        lastReleasedHandle = handle;
    }

    void setGpuSurface(void* handle, uint64_t seq = 1) {
        m_surface.kind = nv::app::FrameSurface::Kind::GpuTexture;
        m_surface.gpuHandle = handle;
        m_surface.seq = seq;
        m_surface.width = 16;
        m_surface.height = 16;
        m_hasNew = true;
    }

    void setCpuSurface(uint64_t seq = 1) {
        m_surface.kind = nv::app::FrameSurface::Kind::CpuRgba;
        m_surface.gpuHandle = nullptr;
        m_surface.seq = seq;
        m_surface.width = 16;
        m_surface.height = 16;
        m_surface.rgba.assign(16 * 16 * 4, 0);
        m_hasNew = true;
    }
};

// QApplication 싱글톤 — 이 TU 전체에서 1회만 생성.
int   g_argc = 0;
char* g_argv[1] = {nullptr};

QApplication& getApp() {
    static QApplication app(g_argc, g_argv);
    return app;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────

TEST_CASE("VideoTileWidget: GpuTexture 서피스 수신 시 releaseConsumed 1회 호출") {
    (void)getApp();
    FakeRegistry reg;
    void* fakeHandle = reinterpret_cast<void*>(0xDEADBEEF);
    reg.setGpuSurface(fakeHandle, /*seq=*/1);

    nv::ui::VideoTileWidget tile(reg, "ch1");
    tile.pollFrame();

    REQUIRE(reg.releaseConsumedCallCount == 1);
    REQUIRE(reg.lastReleasedHandle == fakeHandle);
}

TEST_CASE("VideoTileWidget: CpuRgba 서피스 수신 시 releaseConsumed 미호출") {
    (void)getApp();
    FakeRegistry reg;
    reg.setCpuSurface(/*seq=*/1);

    nv::ui::VideoTileWidget tile(reg, "ch1");
    tile.pollFrame();

    REQUIRE(reg.releaseConsumedCallCount == 0);
}

TEST_CASE("VideoTileWidget: 새 프레임 없으면 releaseConsumed 미호출") {
    (void)getApp();
    FakeRegistry reg;
    // m_hasNew = false — latestSurface 항상 false 반환

    nv::ui::VideoTileWidget tile(reg, "ch1");
    tile.pollFrame();

    REQUIRE(reg.releaseConsumedCallCount == 0);
}

// ── C3 계약: RHI 렌더러가 GpuTexture 핸들 수명을 소유 ────────────────────────────
// 오프스크린(GPU/브리지 미준비) 환경에서 RhiVideoRenderer.present(GpuTexture)는 zero-copy
// map을 시도하지 못하므로(브리지 미초기화) RGBA 폴백으로 떨어지고, 받은 핸들을 즉시 반납해야
// 누수가 없다 — 정확히 1회. CpuRgba 서피스는 핸들이 없으므로 반납 호출이 없어야 한다.
TEST_CASE("RhiVideoRenderer: 브리지 미준비 GpuTexture 핸들 정확히 1회 반납") {
    (void)getApp();
    FakeRegistry reg;
    void* fakeHandle = reinterpret_cast<void*>(0xABCDEF01);

    nv::ui::RhiVideoRenderer renderer(nullptr, &reg, "chR");

    nv::app::FrameSurface s;
    s.kind = nv::app::FrameSurface::Kind::GpuTexture;
    s.gpuHandle = fakeHandle;
    s.seq = 7;
    s.width = 16;
    s.height = 16;
    s.rgba.assign(16 * 16 * 4, 0);   // 동반 RGBA(폴백 표시용)

    renderer.present(s);

    REQUIRE(reg.releaseConsumedCallCount == 1);
    REQUIRE(reg.lastReleasedHandle == fakeHandle);
}

TEST_CASE("RhiVideoRenderer: CpuRgba 서피스는 releaseConsumed 미호출") {
    (void)getApp();
    FakeRegistry reg;

    nv::ui::RhiVideoRenderer renderer(nullptr, &reg, "chR");

    nv::app::FrameSurface s;
    s.kind = nv::app::FrameSurface::Kind::CpuRgba;
    s.seq = 3;
    s.width = 16;
    s.height = 16;
    s.rgba.assign(16 * 16 * 4, 0);

    renderer.present(s);

    REQUIRE(reg.releaseConsumedCallCount == 0);
}

// ── RHI 프로브 캐시 동작 (#20) ─────────────────────────────────────────────────
// 오프스크린(QPA=offscreen) 환경에서는 QRhi 초기화가 실패해 첫 타일 생성 시
// renderFailed → fallbackToSw() 가 발생하고 프로세스 전역 캐시가 "RHI 불가"로
// 기록된다. 이 테스트는 그 캐시 결과를 이용해 이후 타일이 정상 동작하는지 확인한다.
//
// 구체적으로: 캐시가 이미 기록된 상태(앞선 테스트에서 RHI 시도→실패)에서도
// GpuTexture 서피스를 pollFrame()으로 받으면 releaseConsumed가 1회 호출돼야 한다.
// (SW 폴백 경로이므로 동반 RGBA를 그리고 GPU 핸들은 즉시 반납.)
TEST_CASE("VideoTileWidget: RHI 프로브 캐시 후 GpuTexture 핸들 정상 반납 (#20)") {
    (void)getApp();
    // 앞선 테스트에서 오프스크린 환경이 이미 프로브를 실행했을 수 있다.
    // 어떤 상태(캐시 hit/miss)이든 정상 동작해야 한다.
    FakeRegistry reg;
    void* fakeHandle = reinterpret_cast<void*>(0xC4C4C4C4);
    reg.setGpuSurface(fakeHandle, /*seq=*/99);
    reg.m_surface.rgba.assign(16 * 16 * 4, 42);  // 동반 RGBA 채움(SW 폴백 표시용)

    nv::ui::VideoTileWidget tile(reg, "ch-probe");
    tile.pollFrame();

    // SW 경로(오프스크린이어서 RHI 캐시 miss 또는 캐시 hit으로 SW 선택)에서
    // GpuTexture 핸들은 즉시 반납해야 한다.
    REQUIRE(reg.releaseConsumedCallCount == 1);
    REQUIRE(reg.lastReleasedHandle == fakeHandle);
}
