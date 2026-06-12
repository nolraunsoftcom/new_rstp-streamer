#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <string>
#include <vector>
#include "src/app/ports/IFrameSurfaceRegistry.h"
#include "src/ui/grid/VideoTileWidget.h"

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
