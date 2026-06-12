#include "FfmpegRecorder.h"

#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
}

namespace nv::infra {

FfmpegRecorder::~FfmpegRecorder() { stop(); }

bool FfmpegRecorder::start(const std::string& path, const AVStream* inputVideoStream) {
    if (m_open) return false;            // 중복 start 금지
    if (inputVideoStream == nullptr) return false;

    cleanup();                           // 이전 부분 상태 잔재 제거 (재진입 안전)
    m_errored = false;
    m_gotKeyframe = false;
    m_firstPts = AV_NOPTS_VALUE;
    m_outStreamIndex = -1;

    // 1) Matroska 출력 컨텍스트 할당. 실패 시 fmt는 nullptr로 남는다.
    AVFormatContext* fmt = nullptr;
    int rc = avformat_alloc_output_context2(&fmt, nullptr, "matroska", path.c_str());
    if (rc < 0 || fmt == nullptr) {
        std::fprintf(stderr, "[FfmpegRecorder] alloc_output_context2 실패 (%d)\n", rc);
        m_errored = true;
        return false;
    }

    // 2) 출력 stream 생성 + codecpar 복사(stream copy, 디코딩 없음).
    AVStream* out = avformat_new_stream(fmt, nullptr);
    if (out == nullptr) {
        std::fprintf(stderr, "[FfmpegRecorder] new_stream 실패\n");
        avformat_free_context(fmt);
        m_errored = true;
        return false;
    }
    if (avcodec_parameters_copy(out->codecpar, inputVideoStream->codecpar) < 0) {
        std::fprintf(stderr, "[FfmpegRecorder] parameters_copy 실패\n");
        avformat_free_context(fmt);
        m_errored = true;
        return false;
    }
    out->codecpar->codec_tag = 0;        // 컨테이너가 적절한 tag를 다시 고르게(remux 정석)
    out->time_base = inputVideoStream->time_base;   // 힌트 — muxer가 최종 결정할 수 있음

    // 3) 파일 열기 (AVFMT_NOFILE muxer가 아니면 pb 필요 — matroska는 파일 기반).
    rc = avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (rc < 0) {
        std::fprintf(stderr, "[FfmpegRecorder] avio_open 실패 (%d): %s\n", rc, path.c_str());
        avformat_free_context(fmt);     // pb는 열리지 않았으므로 free만
        m_errored = true;
        return false;
    }

    // 4) 헤더 쓰기. 실패 시 pb를 닫고 컨텍스트 해제.
    rc = avformat_write_header(fmt, nullptr);
    if (rc < 0) {
        std::fprintf(stderr, "[FfmpegRecorder] write_header 실패 (%d)\n", rc);
        avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        m_errored = true;
        return false;
    }

    m_fmt = fmt;
    m_outStreamIndex = out->index;
    m_inTimeBase = inputVideoStream->time_base;
    m_open = true;
    return true;
}

void FfmpegRecorder::writePacket(const AVPacket* pkt) {
    if (!m_open || m_fmt == nullptr || pkt == nullptr) return;
    // 부채 해소: 한 번 쓰기 오류가 나면 이후 패킷도 거의 확실히 실패한다 — 매 패킷 재시도하면
    // av_interleaved_write_frame 실패 + stderr 로그가 폭주한다(로그 스팸). 오류 상태면 조기 반환.
    if (m_errored) return;

    // 첫 패킷은 키프레임이어야 한다 — 키프레임 전 패킷은 드랍(remux 디코드 불가 방지).
    if (!m_gotKeyframe) {
        if ((pkt->flags & AV_PKT_FLAG_KEY) == 0) return;
        m_gotKeyframe = true;
        m_firstPts = pkt->pts;           // 0 오프셋 기준(AV_NOPTS_VALUE이면 오프셋 미적용)
    }

    // 원본 pkt를 변경하지 않도록 복사본에 작업한다(다른 소비자=디코더와 공유될 수 있음).
    AVPacket* copy = av_packet_alloc();
    if (copy == nullptr) { m_errored = true; return; }
    if (av_packet_ref(copy, pkt) < 0) {
        av_packet_free(&copy);
        m_errored = true;
        return;
    }

    // 첫 키프레임 pts를 0으로 당겨 세그먼트가 0부터 시작하게 한다(입력 timebase 상에서).
    if (m_firstPts != AV_NOPTS_VALUE) {
        if (copy->pts != AV_NOPTS_VALUE) copy->pts -= m_firstPts;
        if (copy->dts != AV_NOPTS_VALUE) copy->dts -= m_firstPts;
    }

    // 입력 timebase → 출력 stream timebase로 pts/dts/duration rescale.
    const AVStream* out = m_fmt->streams[m_outStreamIndex];
    av_packet_rescale_ts(copy, m_inTimeBase, out->time_base);
    copy->stream_index = m_outStreamIndex;
    copy->pos = -1;                      // muxer가 위치를 다시 계산

    const int rc = av_interleaved_write_frame(m_fmt, copy);
    if (rc < 0) {
        // 격리: 쓰기 실패는 이 레코더만 오류 상태로 — 예외 던지지 않음, 호출측(디코드 스레드)
        // 으로 전파하지 않는다. 이후 패킷은 계속 시도하되 hadError()로 진단 가능.
        std::fprintf(stderr, "[FfmpegRecorder] write_frame 실패 (%d)\n", rc);
        m_errored = true;
    }
    // av_interleaved_write_frame은 성공 시 packet 소유권을 가져가 내부에서 ref를 비운다.
    // 실패 시에도 unref하므로 여기서는 free만 하면 된다.
    av_packet_free(&copy);
}

void FfmpegRecorder::stop() {
    if (m_fmt == nullptr) {              // 미시작/이미 정리됨 — 멱등
        m_open = false;
        return;
    }

    if (m_open) {
        // 헤더가 쓰였을 때만 트레일러를 쓴다(write_header 성공 후에만 m_open=true).
        const int rc = av_write_trailer(m_fmt);
        if (rc < 0) {
            std::fprintf(stderr, "[FfmpegRecorder] write_trailer 실패 (%d)\n", rc);
            m_errored = true;
        }
    }
    cleanup();
}

void FfmpegRecorder::cleanup() {
    if (m_fmt != nullptr) {
        // pb를 먼저 닫는다(avformat_free_context가 pb를 해제하지 않으므로 누수 방지).
        if (m_fmt->pb != nullptr) avio_closep(&m_fmt->pb);
        avformat_free_context(m_fmt);
        m_fmt = nullptr;
    }
    m_open = false;
    m_outStreamIndex = -1;
    m_gotKeyframe = false;
    m_firstPts = AV_NOPTS_VALUE;
}

} // namespace nv::infra
