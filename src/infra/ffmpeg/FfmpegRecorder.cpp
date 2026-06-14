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

    // 3·4) avio_open + write_header는 첫 키프레임 도착 시점으로 지연한다(writePacket에서 수행).
    // 키프레임 전 hard-kill 시 빈 헤더 파일(~542B)을 남기지 않는다.

    m_fmt = fmt;
    m_path = path;
    m_outStreamIndex = out->index;
    m_inTimeBase = inputVideoStream->time_base;
    m_headerWritten = false;
    m_open = true;

    // D8: 전용 쓰기 스레드 기동. 디코드 스레드는 큐에 넣기만 하고, 이 스레드가 mux한다.
    {
        std::lock_guard lk(m_qMu);
        m_writerStop = false;
        m_droppedPackets = 0;
    }
    m_writer = std::thread(&FfmpegRecorder::writerLoop, this);
    return true;
}

void FfmpegRecorder::writePacket(const AVPacket* pkt) {
    if (!m_open || m_fmt == nullptr || pkt == nullptr) return;
    // 부채 해소: 한 번 쓰기 오류가 나면 이후 패킷도 거의 확실히 실패한다 — 매 패킷 큐잉하면
    // 의미 없이 메모리만 쓴다. 오류 상태면 조기 반환(로그 스팸도 방지).
    if (m_errored.load()) return;

    // 첫 패킷은 키프레임이어야 한다 — 키프레임 전 패킷은 드랍(remux 디코드 불가 방지).
    if (!m_gotKeyframe) {
        if ((pkt->flags & AV_PKT_FLAG_KEY) == 0) return;
        m_gotKeyframe = true;
        m_firstPts = pkt->pts;           // 0 오프셋 기준(AV_NOPTS_VALUE이면 오프셋 미적용)
    }

    // 첫 키프레임 도착 시점에 비로소 파일을 연다 — 키프레임 전 강제종료 시 빈 헤더 파일을 남기지 않는다.
    if (!m_headerWritten) {
        int rc = avio_open(&m_fmt->pb, m_path.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            std::fprintf(stderr, "[FfmpegRecorder] avio_open 실패 (%d): %s\n", rc, m_path.c_str());
            m_errored.store(true);
            return;
        }
        rc = avformat_write_header(m_fmt, nullptr);
        if (rc < 0) {
            std::fprintf(stderr, "[FfmpegRecorder] write_header 실패 (%d)\n", rc);
            avio_closep(&m_fmt->pb);
            m_errored.store(true);
            return;
        }
        m_headerWritten = true;
    }

    // 원본 pkt를 변경하지 않도록 복사본에 작업한다(다른 소비자=디코더와 공유될 수 있음).
    AVPacket* copy = av_packet_alloc();
    if (copy == nullptr) { m_errored.store(true); return; }
    if (av_packet_ref(copy, pkt) < 0) {
        av_packet_free(&copy);
        m_errored.store(true);
        return;
    }

    // 첫 키프레임 pts를 0으로 당겨 세그먼트가 0부터 시작하게 한다(입력 timebase 상에서).
    if (m_firstPts != AV_NOPTS_VALUE) {
        if (copy->pts != AV_NOPTS_VALUE) copy->pts -= m_firstPts;
        if (copy->dts != AV_NOPTS_VALUE) copy->dts -= m_firstPts;
    }

    // 입력 timebase → 출력 stream timebase로 pts/dts/duration rescale.
    // (rescale/오프셋은 디코드 스레드에서 수행 — 가볍다. 블로킹 mux만 쓰기 스레드로 격리.)
    const AVStream* out = m_fmt->streams[m_outStreamIndex];
    av_packet_rescale_ts(copy, m_inTimeBase, out->time_base);
    copy->stream_index = m_outStreamIndex;
    copy->pos = -1;                      // muxer가 위치를 다시 계산

    // D8: 인라인 write 대신 큐에 넣고 즉시 반환(디코드 핫루프 비블로킹).
    // 큐가 가득이면 가장 오래된 비키프레임을 드랍(GOP 단위, 단기 녹화라 수용).
    {
        std::lock_guard lk(m_qMu);
        if (m_queue.size() >= kMaxQueue) {
            dropOldestNonKeyLocked();
        }
        m_queue.push_back(copy);   // 소유 이전 — 쓰기 스레드가 free
    }
    m_qCv.notify_one();
}

// 쓰기 스레드: 큐에서 패킷을 꺼내 mux. 블로킹 디스크 I/O를 디코드 스레드에서 격리한다.
void FfmpegRecorder::writerLoop() {
    for (;;) {
        AVPacket* pkt = nullptr;
        {
            std::unique_lock lk(m_qMu);
            m_qCv.wait(lk, [this] { return !m_queue.empty() || m_writerStop; });
            if (m_queue.empty()) {
                // 큐가 비었고 종료 신호 — flush 완료, 스레드 종료.
                if (m_writerStop) return;
                continue;
            }
            pkt = m_queue.front();
            m_queue.pop_front();
        }
        // mux는 락 밖에서(블로킹 I/O 동안 디코드 스레드의 enqueue를 막지 않음).
        if (!m_errored.load()) {
            const int rc = av_interleaved_write_frame(m_fmt, pkt);
            if (rc < 0) {
                // 격리: 쓰기 실패는 이 레코더만 오류 상태로. 디코드 스레드로 전파하지 않는다.
                std::fprintf(stderr, "[FfmpegRecorder] write_frame 실패 (%d)\n", rc);
                m_errored.store(true);
            }
        }
        // 성공 시 av_interleaved_write_frame이 ref를 비우고, 실패/오류스킵 시에도 free로 정리.
        av_packet_free(&pkt);
    }
}

void FfmpegRecorder::stopWriter() {
    if (!m_writer.joinable()) return;
    {
        std::lock_guard lk(m_qMu);
        m_writerStop = true;   // 큐를 다 비운 뒤 종료(flush 보장)
    }
    m_qCv.notify_one();
    m_writer.join();
}

void FfmpegRecorder::dropOldestNonKeyLocked() {
    // 가장 오래된 비키프레임을 찾아 드랍(GOP 보존 노력). 모두 키프레임이면 맨 앞을 드랍.
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (((*it)->flags & AV_PKT_FLAG_KEY) == 0) {
            av_packet_free(&*it);
            m_queue.erase(it);
            ++m_droppedPackets;
            return;
        }
    }
    AVPacket* oldest = m_queue.front();
    m_queue.pop_front();
    av_packet_free(&oldest);
    ++m_droppedPackets;
}

void FfmpegRecorder::stop() {
    // D8: 먼저 쓰기 스레드를 멈추고 join한다 — 큐의 잔여 패킷이 모두 mux된 뒤 트레일러를 쓴다.
    // (writerLoop은 m_writerStop이어도 큐를 다 비우고 종료하므로 flush가 보장된다.)
    stopWriter();

    // 큐 드랍 통계 1회 로그(있을 때만). m_writer join 후라 락 없이 안전하지만 일관성 위해 잠근다.
    uint64_t dropped = 0;
    {
        std::lock_guard lk(m_qMu);
        dropped = m_droppedPackets;
        // 혹시 남은 패킷(오류 경로 등) 정리 — 누수 방지.
        for (AVPacket* p : m_queue) av_packet_free(&p);
        m_queue.clear();
    }
    if (dropped > 0) {
        std::fprintf(stderr, "[FfmpegRecorder] 큐 가득으로 %llu 패킷 드랍(느린 저장소)\n",
                     static_cast<unsigned long long>(dropped));
    }

    if (m_fmt == nullptr) {              // 미시작/이미 정리됨 — 멱등
        m_open = false;
        return;
    }

    if (m_headerWritten) {
        // 헤더가 실제로 쓰인 경우(첫 키프레임 도달 후)에만 트레일러를 쓴다.
        // m_headerWritten=false이면 avio_open도 하지 않았으므로 파일 자체가 없다.
        const int rc = av_write_trailer(m_fmt);
        if (rc < 0) {
            std::fprintf(stderr, "[FfmpegRecorder] write_trailer 실패 (%d)\n", rc);
            m_errored.store(true);
        }
    }
    cleanup();
}

void FfmpegRecorder::cleanup() {
    if (m_fmt != nullptr) {
        // pb를 먼저 닫는다(avformat_free_context가 pb를 해제하지 않으므로 누수 방지).
        // m_headerWritten=false인 경우 pb는 열리지 않았으므로 avio_closep은 nullptr를 받아 무동작.
        if (m_fmt->pb != nullptr) avio_closep(&m_fmt->pb);
        avformat_free_context(m_fmt);
        m_fmt = nullptr;
    }
    m_open = false;
    m_headerWritten = false;
    m_path.clear();
    m_outStreamIndex = -1;
    m_gotKeyframe = false;
    m_firstPts = AV_NOPTS_VALUE;
}

} // namespace nv::infra
