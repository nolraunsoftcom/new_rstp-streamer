#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "src/app/MarshallingStreamSource.h"
#include "tests/helpers/FakeStreamSource.h"

using namespace nv::app;
using namespace nv::test;

namespace {
// 동기 가짜 실행기: post 즉시 실행하지 않고 큐에 쌓아 직렬화 검증을 가능케 한다.
struct QueueExecutor final : IExecutor {
    std::vector<std::function<void()>> q;
    void post(std::function<void()> fn) override { q.push_back(std::move(fn)); }
    void runAll() { auto copy = std::move(q); q.clear(); for (auto& f : copy) f(); }
};

struct RecordingListener final : StreamSourceListener {
    std::vector<std::string> events;
    void onSessionOpened() override { events.push_back("session"); }
    void onPacketReceived() override { events.push_back("packet"); }
    void onFrameDecoded() override { events.push_back("decoded"); }
    void onFramePresented() override { events.push_back("presented"); }
    void onSourceError(nv::domain::DiagnosisReason r) override {
        events.push_back(std::string("error:") + std::string(toString(r)));
    }
};
}

TEST_CASE("이벤트는 즉시 실행되지 않고 executor를 거쳐 전달된다") {
    FakeStreamSource inner;
    QueueExecutor ex;
    RecordingListener real;
    MarshallingStreamSource src(inner, ex);

    src.open("rtsp://x", real);
    REQUIRE(inner.listener() != nullptr);

    inner.listener()->onSessionOpened();           // 어댑터 스레드 흉내
    inner.listener()->onSourceError(nv::domain::DiagnosisReason::NoPackets);
    CHECK(real.events.empty());                    // 아직 전달 안 됨 (직렬화 대기)

    ex.runAll();                                   // control 스레드 차례
    REQUIRE(real.events.size() == 2);
    CHECK(real.events[0] == "session");
    CHECK(real.events[1] == "error:NoPackets");
}

TEST_CASE("close 후 늦게 실행된 이벤트도 안전하다 (리스너는 close 이후 큐에 남아있을 수 있음)") {
    FakeStreamSource inner;
    QueueExecutor ex;
    RecordingListener real;
    MarshallingStreamSource src(inner, ex);

    src.open("rtsp://x", real);
    inner.listener()->onPacketReceived();          // 큐에 들어감
    src.close();
    ex.runAll();                                   // close 이후 실행 — UB 없이 전달되어야 함
    REQUIRE(real.events.size() == 1);              // 수신측(ChannelController)의
    CHECK(real.events[0] == "packet");             // m_sourceAlive 가드가 무시 책임을 진다
}
