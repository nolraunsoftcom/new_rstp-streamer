#pragma once
#include <memory>
#include <string>
#include "ports/IExecutor.h"
#include "ports/IStreamSource.h"

namespace nv::app {

// IStreamSource 데코레이터: 어댑터(임의 스레드)의 리스너 이벤트를 IExecutor로 직렬화한다.
// 최종 리스너(ChannelController)는 반드시 executor 스레드에서만 호출된다 (설계 D6).
// 수명: 프록시는 shared_ptr로 잡혀 큐에 남은 이벤트가 close 이후 실행돼도 안전하다.
// close 이후 도착하는 늦은 이벤트의 무시는 수신측 m_sourceAlive 가드 책임.
class MarshallingStreamSource final : public IStreamSource {
public:
    MarshallingStreamSource(IStreamSource& inner, IExecutor& executor)
        : m_inner(inner), m_executor(executor) {}

    void open(const std::string& url, StreamSourceListener& listener) override {
        m_proxy = std::make_shared<Proxy>(m_executor, listener);
        m_inner.open(url, *m_proxy);
    }

    void close() override {
        m_inner.close();   // 어댑터 스레드 합류까지 블로킹 → 이후 새 이벤트 없음
        m_proxy.reset();   // 큐에 남은 이벤트는 shared_ptr 캡처로 생존
    }

private:
    struct Proxy final : StreamSourceListener,
                         std::enable_shared_from_this<Proxy> {
        Proxy(IExecutor& ex, StreamSourceListener& real) : ex(ex), real(real) {}
        IExecutor& ex;
        StreamSourceListener& real;

        void onSessionOpened() override {
            ex.post([self = shared_from_this()] { self->real.onSessionOpened(); });
        }
        void onPacketReceived() override {
            ex.post([self = shared_from_this()] { self->real.onPacketReceived(); });
        }
        void onFrameDecoded() override {
            ex.post([self = shared_from_this()] { self->real.onFrameDecoded(); });
        }
        void onFramePresented() override {
            ex.post([self = shared_from_this()] { self->real.onFramePresented(); });
        }
        void onSourceError(nv::domain::DiagnosisReason r) override {
            ex.post([self = shared_from_this(), r] { self->real.onSourceError(r); });
        }
        void onBytesReceived(long long bytes) override {
            ex.post([self = shared_from_this(), bytes] { self->real.onBytesReceived(bytes); });
        }
        void onFrameDropped() override {
            ex.post([self = shared_from_this()] { self->real.onFrameDropped(); });
        }
    };

    IStreamSource& m_inner;
    IExecutor& m_executor;
    std::shared_ptr<Proxy> m_proxy;
};

} // namespace nv::app
