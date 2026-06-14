#include "RecordingController.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

// 순수 app 레이어 — Qt/FFmpeg include 없음.
// 경로 생성은 C++ stdlib(chrono + ctime + sstream)만 사용.
namespace nv::app {

// 채널명에서 파일명에 안전하지 않은 문자를 '_'로 치환한다.
static std::string sanitizeName(const std::string& name) {
    std::string s = name.empty() ? "channel" : name;
    for (char& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' ||
            static_cast<unsigned char>(c) < 0x20) {
            c = '_';
        }
    }
    return s;
}

RecordingController::RecordingController(IRecordingSink& sink,
                                         IClock& clock,
                                         ILogger& logger,
                                         nv::domain::SegmentPolicy policy)
    : m_sink(sink), m_clock(clock), m_logger(logger), m_policy(policy) {}

void RecordingController::setObserver(RecordingObserver observer) {
    m_observer = std::move(observer);
}

nv::domain::RecordingState RecordingController::stateOf(const std::string& channelId) const {
    auto it = m_channels.find(channelId);
    return it == m_channels.end() ? nv::domain::RecordingState::Idle : it->second.state;
}

// ── 내부 헬퍼 ───────────────────────────────────────────────────────────────

std::string RecordingController::makePath(const std::string& channelName) const {
    // 타임스탬프: chrono time_point → time_t → localtime → strftime
    const auto tp = m_clock.now();
    // steady_clock은 time_t 변환 불가 → system_clock 기준 오프셋으로 조회.
    // (steady_clock epoch는 구현 정의라 time_t 변환 불가 — system_clock 사용.)
    (void)tp;  // makePath는 system_clock을 직접 호출해 타임스탬프를 생성
    const std::time_t t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());

    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts[20];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_buf);

    // D2 충돌 방지: 같은 초 안의 stop→start(롤오버/재연결)는 동일 타임스탬프라 경로가
    // 겹쳐 avio_open이 직전 세그먼트를 0바이트로 truncate한다. 단조 증가 시퀀스를 접미사로
    // 붙여 같은 초라도 항상 다른 경로를 보장한다(..._HHmmss_NNN). 시퀀스는 컨트롤러 수명 동안
    // 단조 증가 — control 스레드 단일 호출이라 atomic 불필요(단순 멤버).
    const unsigned seq = m_pathSeq++;
    char seqbuf[8];
    std::snprintf(seqbuf, sizeof(seqbuf), "_%03u", seq % 1000);

    // 저장 디렉토리: 환경변수 NV_RECORD_DIR 또는 빈 문자열(cwd 상대).
    // 실제 앱은 infra가 main()에서 NV_RECORD_DIR을 QStandardPaths로 채워 줘도 되고,
    // ChannelSourceFactory::startRecording이 경로를 덮어써도 된다.
    // 테스트에서는 /tmp 또는 빈 디렉토리 사용.
    std::string dir;
    if (const char* env = std::getenv("NV_RECORD_DIR")) {
        dir = env;
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    }

    return dir + sanitizeName(channelName) + '_' + ts + seqbuf + ".mkv";
}

void RecordingController::notify(const std::string& channelId, nv::domain::RecordingState s) {
    if (m_observer) m_observer(channelId, s);
}

void RecordingController::doStart(const std::string& channelId, const std::string& channelName) {
    const std::string path = makePath(channelName);
    const bool ok = m_sink.startRecording(channelId, path);
    if (!ok) {
        // D8 격리: 이 채널만 Idle 유지, 경고 로그. 다른 채널·재생 무영향.
        m_logger.log(LogLevel::Warn, channelId, "RecordingController",
                     "startRecording 실패 — Idle 유지: " + path);
        auto& ch = m_channels[channelId];
        ch.state = nv::domain::RecordingState::Idle;
        ch.channelName = channelName;

        // D2 백오프: 연속 실패를 누적한다. 임계치 도달 시 armed를 내려 무한 재시도·로그
        // 스팸을 막고 1회 명확한 경고를 남긴다(사용자 재시도 필요). tick/onStreaming의
        // 재시도 경로(D1)가 이 카운터를 공유하므로 영구 실패가 폭주하지 않는다.
        if (++ch.startFailures >= kMaxStartFailures && ch.armed) {
            ch.armed = false;
            m_logger.log(LogLevel::Warn, channelId, "RecordingController",
                         "녹화 시작 반복 실패 — 중단, 사용자 재시도 필요");
        }
        notify(channelId, nv::domain::RecordingState::Idle);
        return;
    }
    auto& ch = m_channels[channelId];
    ch.channelName  = channelName;
    ch.segmentStart = m_clock.now();
    ch.startFailures = 0;   // D2: 성공 시 백오프 카운터 리셋
    ch.retryStart = false;  // D1: 정상 녹화 중 — 재시도 게이트 해제
    // P4d: sink.isRecording이 이미 true(예: 테스트용 페이크)면 즉시 Recording,
    // false이면 Starting(요청됨, 첫 키프레임 대기) — tick()이 전환을 완료한다.
    if (m_sink.isRecording(channelId)) {
        ch.state = nv::domain::RecordingState::Recording;
        notify(channelId, nv::domain::RecordingState::Recording);
    } else {
        ch.state = nv::domain::RecordingState::Starting;
        notify(channelId, nv::domain::RecordingState::Starting);
    }
}

void RecordingController::doStop(const std::string& channelId) {
    m_sink.stopRecording(channelId);
    auto& ch  = m_channels[channelId];
    ch.state  = nv::domain::RecordingState::Idle;
    ch.retryStart = false;   // D1: 명시적 stop(토글/드롭) — tick 재시도 게이트 해제
    notify(channelId, nv::domain::RecordingState::Idle);
}

// ── 공개 API ─────────────────────────────────────────────────────────────────

void RecordingController::toggle(const std::string& channelId,
                                 const std::string& channelName) {
    auto it = m_channels.find(channelId);
    const bool active = (it != m_channels.end()) &&
        (it->second.armed ||
         it->second.state == nv::domain::RecordingState::Recording ||
         it->second.state == nv::domain::RecordingState::Starting);
    if (active) {
        // 녹화 의도 해제 + 현재 세그먼트 종료
        m_channels[channelId].armed = false;
        doStop(channelId);
    } else {
        // 녹화 의도 설정 + 첫 세그먼트 즉시 시작(즉각적 시작감)
        m_channels[channelId].armed = true;
        doStart(channelId, channelName);
    }
}

void RecordingController::onChannelRemoved(const std::string& channelId) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return;
    it->second.armed = false;
    if (it->second.state == nv::domain::RecordingState::Recording ||
        it->second.state == nv::domain::RecordingState::Starting) {
        // best-effort: 소스가 곧 파괴되므로 실패 가능, 유령 상태 방지가 핵심
        m_sink.stopRecording(channelId);
        notify(channelId, nv::domain::RecordingState::Idle);
    }
    m_channels.erase(it);
}

void RecordingController::onReconnect(const std::string& channelId,
                                      const std::string& /*channelName*/) {
    if (!m_policy.splitOnReconnect) return;
    auto it = m_channels.find(channelId);
    if (it == m_channels.end() || !it->second.armed ||
        (it->second.state != nv::domain::RecordingState::Recording &&
         it->second.state != nv::domain::RecordingState::Starting)) return;

    // 드롭 엣지: 죽어가는 소스에 doStart 하지 않는다. 현재 세그먼트만 종료하고
    // armed는 유지 — 복구(onStreaming) 시 새 세그먼트가 시작된다.
    doStop(channelId);
}

void RecordingController::onStreaming(const std::string& channelId,
                                      const std::string& channelName) {
    auto it = m_channels.find(channelId);
    // armed가 아니면(사용자가 녹화 의도 없음) 무동작
    if (it == m_channels.end() || !it->second.armed) return;
    // 이미 녹화 중이거나 Starting(키프레임 대기 중)이면 중복 시작 방지
    if (it->second.state == nv::domain::RecordingState::Recording ||
        it->second.state == nv::domain::RecordingState::Starting) return;

    // armed인데 미녹화(드롭으로 종료됐거나 수렴으로 떨어짐) → 새 세그먼트 시작
    doStart(channelId, channelName);
}

void RecordingController::tick() {
    const auto now = m_clock.now();
    for (auto& [id, ch] : m_channels) {
        // ── D1 롤오버 재시도: armed인데 Idle로 떨어진 채널을 복구 ──────────────────
        // 롤오버 doStart 실패(디스크 일시 풀)나 디스크오류 수렴으로 armed && Idle이 되면,
        // onStreaming 신호가 없어도(이미 Streaming 상태라 전이 엣지가 안 옴) 영원히
        // 녹화가 멈춘다. retryStart 게이트가 선 채널만 tick에서 doStart를 재시도해 디스크가
        // 비워지면 자동 복구한다. onReconnect 드롭 엣지(소스 재연결 대기)는 retryStart=false라
        // 죽은 소스에 재시도하지 않는다. D2 백오프(doStart 내부)가 영구 실패 폭주를 막는다.
        if (ch.armed && ch.state == nv::domain::RecordingState::Idle && ch.retryStart) {
            doStart(id, ch.channelName);
            continue;
        }

        // P4d: Starting → Recording 전환: 첫 키프레임으로 sink가 실제 녹화를 시작했는지 확인.
        if (ch.state == nv::domain::RecordingState::Starting) {
            if (m_sink.isRecording(id)) {
                ch.state = nv::domain::RecordingState::Recording;
                notify(id, nv::domain::RecordingState::Recording);
            } else {
                // D3 유예와 동일 창: kReconcileGrace 이후에도 isRecording==false이면
                // 디코드 스레드 start 실패로 간주, Idle로 수렴(UI REC 해제, armed 유지).
                static constexpr std::chrono::seconds kReconcileGrace{3};
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(now - ch.segmentStart);
                if (elapsed >= kReconcileGrace) {
                    m_logger.log(LogLevel::Warn, id, "RecordingController",
                                 "Starting 상태에서 녹화 미확인 — Idle로 수렴(REC 해제)");
                    m_sink.stopRecording(id);
                    ch.state = nv::domain::RecordingState::Idle;
                    notify(id, nv::domain::RecordingState::Idle);
                }
            }
            continue;
        }

        if (ch.state != nv::domain::RecordingState::Recording) continue;

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(now - ch.segmentStart);

        // D3 수렴(reconciliation): sink.startRecording 성공은 래치 설정일 뿐,
        // 디코드 스레드의 FfmpegRecorder::start가 실패하면 sink는 녹화를 닫고
        // isRecording==false가 되지만 컨트롤러는 영원히 Recording이라 UI REC 뱃지가
        // 거짓으로 남는다. 컨트롤러가 Recording인데 sink가 더 이상 녹화 중이 아니면
        // Idle로 수렴 + 경고 로그 + 옵저버 통지(UI REC 해제)한다.
        //
        // 유예(kReconcileGrace): 디코드 스레드는 다음 키프레임에서야 레코더를 연다 →
        // start 직후 첫 tick에는 sink.isRecording이 아직 false일 수 있다(정상 비동기 지연).
        // 유예 시간 안에는 수렴하지 않아 거짓 해제를 막는다. 유예 후에도 false면 진짜 실패.
        static constexpr std::chrono::seconds kReconcileGrace{3};
        if (elapsed >= kReconcileGrace && !m_sink.isRecording(id)) {
            m_logger.log(LogLevel::Warn, id, "RecordingController",
                         "녹화 중이라 표시됐으나 sink는 비녹화 — Idle로 수렴(REC 해제)");
            // H2: stopRecording을 명시적으로 호출해 FfmpegStreamSource의
            // m_recordRequested 래치를 확실히 내린다. wedge-detach 등 예외 경로에서
            // 좀비 녹화(레코더는 닫혔으나 래치가 살아있는 상태)를 차단한다.
            // armed는 유지 — H1 적용 후 여기 도달은 진짜 시작 실패이므로
            // 다음 tick(D1)/onStreaming 시 재시도가 맞다(로그로 가시화됨, D2 백오프로 제한).
            m_sink.stopRecording(id);
            ch.state = nv::domain::RecordingState::Idle;
            notify(id, nv::domain::RecordingState::Idle);
            continue;
        }

        // D10 디스크/쓰기 오류 가시화: sink는 여전히 isRecording==true(m_open 유지)지만
        // 레코더가 쓰기 오류(디스크 풀 등)를 만나면 데이터가 무음으로 손실된다. UI는 REC를
        // 표시하나 실제 저장은 멈춘 상태 — 최대 세그먼트 길이(10분)까지 지속될 수 있다.
        // hasRecordingError로 이를 3초 내 즉시 가시화한다: 녹화를 멈추고 Idle로 수렴 + 경고
        // 로그 + 옵저버 통지(UI REC 해제). armed는 유지 → D1 재시도가 디스크 복구 시 자동 재개.
        if (m_sink.hasRecordingError(id)) {
            m_logger.log(LogLevel::Warn, id, "RecordingController",
                         "디스크/쓰기 오류로 녹화 중단 — Idle로 수렴(REC 해제)");
            m_sink.stopRecording(id);
            ch.state = nv::domain::RecordingState::Idle;
            // D10 백오프: 디스크오류 수렴은 doStart가 매번 "성공"(래치)이라 startFailures를
            // 리셋해버려 무한 churn이 된다. 별도 카운터로 연속 디스크오류를 누적해 임계 도달 시
            // armed를 내려 churn(매 사이클 새 .mkv 생성 + 쓰기스레드 spawn/join)을 멈춘다.
            if (++ch.diskErrors >= kMaxDiskErrors && ch.armed) {
                ch.armed = false;
                ch.retryStart = false;   // 재시도 중단 — 디스크가 비워질 때까지 멈춤
                m_logger.log(LogLevel::Warn, id, "RecordingController",
                             "디스크/쓰기 오류 반복 — 녹화 중단, 디스크 공간 확인 필요");
            } else {
                ch.retryStart = true;   // D1: 디스크 복구 시 tick이 새 세그먼트로 자동 재개
            }
            notify(id, nv::domain::RecordingState::Idle);
            continue;
        }

        // 확인된 정상 세그먼트: 여기 도달 = Recording && 윈도우 경과 && isRecording && 무오류.
        // 윈도우는 grace(3s)가 아니라 kDiskHealthyResetWindow(30s) — avio 버퍼 윈도우(수초)에
        // 거짓 리셋되어 백오프가 무력화되는 것을 막는다. 진짜 지속 녹화만 디스크오류 백오프를
        // 리셋한다(장시간 운영 중 간헐 오류 누적으로 인한 거짓 disarm도 방지).
        if (elapsed >= kDiskHealthyResetWindow && m_sink.isRecording(id)) {
            ch.diskErrors = 0;
        }

        if (elapsed >= m_policy.maxDuration) {
            // 롤오버: stop + 새 경로로 start. doStart 실패 시 armed && Idle로 떨어지지만
            // retryStart를 세워 다음 tick의 D1 재시도가 복구한다(이전엔 영구 정지였음).
            m_sink.stopRecording(id);
            ch.state = nv::domain::RecordingState::Idle;  // 임시 — doStart가 Recording으로 전환
            ch.retryStart = true;
            doStart(id, ch.channelName);
        }
    }
}

} // namespace nv::app
