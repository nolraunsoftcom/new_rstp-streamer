#pragma once
#include <string>
#include "src/domain/health/DiagnosisReason.h"

namespace nv::app {

// 어댑터(FFmpeg 등)가 도메인 이벤트로 번역해 호출하는 리스너 (설계 D4).
// M1-코어에서는 동기 호출 가정 — 호출 직렬화(전용 control 스레드 마샬링)는
// M1-파이프라인에서 어댑터 경계에 추가한다.
class StreamSourceListener {
public:
    virtual ~StreamSourceListener() = default;
    virtual void onSessionOpened() = 0;
    virtual void onPacketReceived() = 0;
    virtual void onFrameDecoded() = 0;
    virtual void onFramePresented() = 0;
    virtual void onSourceError(nv::domain::DiagnosisReason reason) = 0;
    // 상태바 지표용(비순수 — 기존 구현체/테스트 무영향). bytes=수신 패킷 크기.
    virtual void onBytesReceived(long long /*bytes*/) {}
    // 디코드/HW전송 실패로 프레임을 표시하지 못함(드롭). 기본 no-op.
    virtual void onFrameDropped() {}
};

class IStreamSource {
public:
    virtual ~IStreamSource() = default;
    virtual void open(const std::string& url, StreamSourceListener& listener) = 0;
    virtual void close() = 0;
};

} // namespace nv::app
