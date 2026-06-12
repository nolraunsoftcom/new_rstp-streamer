#include "FfmpegStreamSource.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cerrno>
#include <vector>

namespace nv::infra {

using nv::domain::DiagnosisReason;

namespace {
DiagnosisReason mapOpenError(int rc) {
    switch (rc) {
        case AVERROR(ETIMEDOUT):
        case AVERROR(EHOSTUNREACH):
        case AVERROR(ENETUNREACH):
        case AVERROR(ECONNREFUSED):
            return DiagnosisReason::DeviceUnreachable;
        default:
            return DiagnosisReason::SessionRefused;   // RTSP 협상/포맷 실패 일반
    }
}
} // namespace

FfmpegStreamSource::FfmpegStreamSource(LatestFrameSlot& frameSlot) : m_frameSlot(frameSlot) {
    avformat_network_init();
}

FfmpegStreamSource::~FfmpegStreamSource() { close(); }

int FfmpegStreamSource::interruptCb(void* opaque) {
    return static_cast<FfmpegStreamSource*>(opaque)->m_stop.load() ? 1 : 0;
}

void FfmpegStreamSource::open(const std::string& url, nv::app::StreamSourceListener& listener) {
    close();                                   // 이전 사이클 정리 (재진입 안전)
    m_stop = false;
    m_thread = std::thread(&FfmpegStreamSource::run, this, url, &listener);
}

void FfmpegStreamSource::close() {
    m_stop = true;                             // interrupt callback이 블로킹 호출을 깨운다
    if (m_thread.joinable()) m_thread.join();
}

void FfmpegStreamSource::run(std::string url, nv::app::StreamSourceListener* listener) {
    AVFormatContext* fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = &FfmpegStreamSource::interruptCb;
    fmt->interrupt_callback.opaque = this;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);   // R4: 로컬 레그 TCP 강제
    av_dict_set(&opts, "timeout", "5000000", 0);      // 소켓 타임아웃 5s(us) — FFmpeg 5+
    av_dict_set(&opts, "stimeout", "5000000", 0);     // 구버전 옵션명 (무시돼도 무해)

    int rc = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0) {                              // 실패 시 fmt는 FFmpeg이 해제한다
        if (!m_stop) listener->onSourceError(mapOpenError(rc));
        return;
    }

    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        if (!m_stop) listener->onSourceError(DiagnosisReason::SessionRefused);
        avformat_close_input(&fmt);
        return;
    }

    const AVCodec* codec = nullptr;
    const int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (vIdx < 0 || codec == nullptr) {
        if (!m_stop) listener->onSourceError(DiagnosisReason::SessionRefused);
        avformat_close_input(&fmt);
        return;
    }

    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[vIdx]->codecpar);
    if (avcodec_open2(dec, codec, nullptr) < 0) {
        if (!m_stop) listener->onSourceError(DiagnosisReason::DecodeError);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return;
    }

    if (!m_stop) listener->onSessionOpened();  // 세션 열림 ≠ 연결 확정 (D1)

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frm = av_frame_alloc();
    SwsContext* sws = nullptr;
    std::vector<uint8_t> rgba;

    while (!m_stop) {
        rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            // 타임아웃/EOF/네트워크 단절 — 패킷이 더 오지 않는다
            if (!m_stop) listener->onSourceError(DiagnosisReason::NoPackets);
            break;
        }
        if (pkt->stream_index != vIdx) {
            av_packet_unref(pkt);
            continue;
        }
        if (!m_stop) listener->onPacketReceived();

        const int sendRc = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (sendRc < 0 && sendRc != AVERROR(EAGAIN)) {
            if (!m_stop) listener->onSourceError(DiagnosisReason::DecodeError);
            break;
        }
        while (avcodec_receive_frame(dec, frm) == 0) {
            if (!m_stop) listener->onFrameDecoded();
            sws = sws_getCachedContext(sws, frm->width, frm->height,
                                       static_cast<AVPixelFormat>(frm->format),
                                       frm->width, frm->height, AV_PIX_FMT_RGBA,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws != nullptr) {
                rgba.resize(static_cast<size_t>(frm->width) * frm->height * 4);
                uint8_t* dst[4] = {rgba.data(), nullptr, nullptr, nullptr};
                int dstStride[4] = {frm->width * 4, 0, 0, 0};
                sws_scale(sws, frm->data, frm->linesize, 0, frm->height, dst, dstStride);
                m_frameSlot.publish(frm->width, frm->height, rgba.data());
            }
            av_frame_unref(frm);
        }
    }

    if (sws != nullptr) sws_freeContext(sws);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}

} // namespace nv::infra
