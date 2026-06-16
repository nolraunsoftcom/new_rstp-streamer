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

TEST_CASE("close 후 큐에 남은 이벤트는 리스너로 전달되지 않고 안전하게 드롭된다") {
    // 계약: close()는 Proxy를 detach해, 큐에 남아 나중에 실행되는 이벤트가 리스너(real)를
    // 역참조하지 못하게 막는다. ChannelManager::removeChannel/updateChannel(relay 전환)는
    // disconnect→close 직후 컨트롤러(=리스너)를 즉시 파괴하므로, close 이후 전달은 use-after-free가
    // 된다. 따라서 "전달 후 리스너가 무시"가 아니라 "전달 자체를 차단"하는 것이 안전 계약이다.
    FakeStreamSource inner;
    QueueExecutor ex;
    RecordingListener real;
    MarshallingStreamSource src(inner, ex);

    src.open("rtsp://x", real);
    inner.listener()->onPacketReceived();          // 큐에 들어감
    src.close();                                   // detach — 잔여 람다는 real을 만지지 않는다
    ex.runAll();                                   // close 이후 실행 — UB 없이 드롭되어야 함
    CHECK(real.events.empty());                    // detach 게이트가 잔여 이벤트를 차단
}

TEST_CASE("리스너가 close 후 파괴돼도 큐 잔여 이벤트 실행이 안전하다 (UAF 방지)") {
    // 실제 버그 재현 형태: 리스너가 close 이후 파괴된 뒤 큐가 비워진다. detach 덕분에 잔여
    // 람다는 (파괴된) 리스너를 역참조하지 않으므로 use-after-free가 발생하지 않는다.
    FakeStreamSource inner;
    QueueExecutor ex;
    MarshallingStreamSource src(inner, ex);

    {
        RecordingListener real;
        src.open("rtsp://x", real);
        inner.listener()->onPacketReceived();      // 큐에 들어감
        inner.listener()->onSessionOpened();
        src.close();                               // detach
    }                                              // real 파괴 — 이후 역참조하면 UAF
    ex.runAll();                                   // detach로 real을 만지지 않으므로 안전
    SUCCEED("close 후 리스너 파괴 + 큐 실행에서 크래시 없음");
}
