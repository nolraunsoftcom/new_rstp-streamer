#pragma once
#include <atomic>
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
        // UAF 방지: close 시점에 리스너(ChannelController)가 곧 파괴될 수 있다 —
        // ChannelManager::removeChannel/updateChannel(relay 전환)는 disconnect→close 직후
        // 곧바로 entry를 erase해 컨트롤러를 파괴한다. 큐에 남은 이벤트는 shared_ptr로 Proxy는
        // 살리지만 Proxy::real(리스너)은 살리지 못하므로, detach로 잔여 이벤트가 real을 만지지
        // 못하게 막는다. close()는 control 스레드(executor)에서만 호출되고 잔여 람다도 같은
        // 스레드에서 실행되므로 여기서 detached=true를 세우면 이후 실행 람다가 반드시 관측한다.
        if (m_proxy) m_proxy->detach();
        m_proxy.reset();   // 외피 ref 해제(큐 잔여 람다는 자체 shared_ptr로 Proxy 생존)
    }

private:
    struct Proxy final : StreamSourceListener,
                         std::enable_shared_from_this<Proxy> {
        Proxy(IExecutor& ex, StreamSourceListener& real) : ex(ex), real(real) {}
        IExecutor& ex;
        StreamSourceListener& real;
        // close() 이후 큐에 남은 이벤트가 (파괴됐을 수 있는) real을 역참조하지 못하게 하는 게이트.
        std::atomic<bool> detached{false};
        void detach() { detached.store(true); }

        void onSessionOpened() override {
            ex.post([self = shared_from_this()] {
                if (!self->detached.load()) self->real.onSessionOpened();
            });
        }
        void onPacketReceived() override {
            ex.post([self = shared_from_this()] {
                if (!self->detached.load()) self->real.onPacketReceived();
            });
        }
        void onFrameDecoded() override {
            ex.post([self = shared_from_this()] {
                if (!self->detached.load()) self->real.onFrameDecoded();
            });
        }
        void onFramePresented() override {
            ex.post([self = shared_from_this()] {
                if (!self->detached.load()) self->real.onFramePresented();
            });
        }
        void onSourceError(nv::domain::DiagnosisReason r) override {
            ex.post([self = shared_from_this(), r] {
                if (!self->detached.load()) self->real.onSourceError(r);
            });
        }
        void onBytesReceived(long long bytes) override {
            ex.post([self = shared_from_this(), bytes] {
                if (!self->detached.load()) self->real.onBytesReceived(bytes);
            });
        }
        void onFrameDropped() override {
            ex.post([self = shared_from_this()] {
                if (!self->detached.load()) self->real.onFrameDropped();
            });
        }
    };

    IStreamSource& m_inner;
    IExecutor& m_executor;
    std::shared_ptr<Proxy> m_proxy;
};

} // namespace nv::app
