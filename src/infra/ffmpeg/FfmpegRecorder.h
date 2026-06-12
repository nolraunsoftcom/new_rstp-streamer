#pragma once
#include <cstdint>
#include <string>

extern "C" {
#include <libavutil/avutil.h>   // AV_NOPTS_VALUE
#include <libavutil/rational.h>
}

struct AVStream;
struct AVPacket;
struct AVFormatContext;
struct AVCodecParameters;

namespace nv::infra {

// 단일 파일 MKV remux 라이터 (설계 D7 크래시 내성 · D8 녹화-재생 격리).
//
// 디코딩 없는 stream copy: 입력 비디오 stream의 codecpar/timebase를 받아 Matroska로
// 그대로 mux한다. 화면에 보이는 바로 그 패킷을 기록하므로 화면=녹화 바이트 일치.
//
// 경계: 이 클래스는 app 포트(IRecordingSink)를 구현하지 않는다 — IRecordingSink는
// channelId/path 수준의 app 명령이고, FfmpegRecorder는 AVPacket/AVStream을 직접 다루는
// infra 내부 협업이다. 배선은 infra(FfmpegStreamSource→FfmpegRecorder, Task3)에서.
//
// 격리: 모든 FFmpeg 오류는 false 반환 + 내부 플래그로 처리하고 예외를 던지지 않는다.
// 디코드 스레드(writePacket 호출측)로 오류가 전파되지 않는다 — 레코더는 자기 상태만 잃는다.
//
// 세그먼트 롤오버: 이 클래스는 단일 파일 remux만 책임진다. 롤오버는 호출측
// (RecordingController, Task4)이 stop() 후 새 경로로 start()를 호출해 구현한다.
//
// 스레드: writePacket은 디코드 스레드에서 인라인으로 불릴 수 있다(remux는 가볍다).
// av_interleaved_write_frame은 디스크 I/O로 블로킹될 수 있으나 M3에서는 인라인 허용.
// 향후 큐잉이 가능하도록 인터페이스는 깔끔히 유지한다(블로킹 측정은 Task6).
// 이 클래스 자체는 스레드 안전하지 않다 — 한 인스턴스는 한 스레드에서만 사용한다.
class FfmpegRecorder {
public:
    FfmpegRecorder() = default;
    ~FfmpegRecorder();

    FfmpegRecorder(const FfmpegRecorder&) = delete;
    FfmpegRecorder& operator=(const FfmpegRecorder&) = delete;

    // MKV 출력을 연다. inputVideoStream의 codecpar/time_base로 출력 stream을 만든다.
    // 성공 시 true(이후 writePacket 가능), 실패 시 false + 부분 자원 완전 정리.
    // 이미 녹화 중이면(중복 start) false.
    bool start(const std::string& path, const AVStream* inputVideoStream);

    // 입력 패킷 하나를 기록한다. 첫 키프레임(AV_PKT_FLAG_KEY) 전 패킷은 드랍한다.
    // pts/dts/duration을 입력 timebase→출력 timebase로 rescale하고 첫 키프레임 pts를
    // 0 기준으로 오프셋한다(세그먼트가 0부터 시작). 원본 pkt는 변경하지 않는다(복사본 사용).
    // 미시작/오류 상태에서는 아무것도 하지 않는다.
    void writePacket(const AVPacket* pkt);

    // 트레일러를 쓰고 출력을 닫는다. 재호출/미시작 안전(멱등).
    void stop();

    bool isRecording() const { return m_open; }

    // 오류 플래그가 한 번이라도 섰는지(격리 진단용). start 성공으로 리셋.
    bool hadError() const { return m_errored; }

private:
    void cleanup();   // 부분/완전 자원 해제 (멱등)

    AVFormatContext* m_fmt = nullptr;
    int m_outStreamIndex = -1;
    AVRational m_inTimeBase{0, 1};   // 입력 stream time_base 보관(rescale용)
    bool m_open = false;
    bool m_gotKeyframe = false;
    bool m_errored = false;
    int64_t m_firstPts = AV_NOPTS_VALUE;   // 첫 키프레임 pts(입력 timebase) — 0 오프셋 기준
};

} // namespace nv::infra
