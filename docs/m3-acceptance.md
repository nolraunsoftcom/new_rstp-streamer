# M3 수용 검수 — 녹화/스냅샷 (증거 수집)

**브랜치:** `m3-features`
**계획:** `docs/superpowers/plans/2026-06-13-m3-recording.md`
**설계 준수:** D3(앱 내 fan-out) · D7(MKV 크래시 내성) · D8(녹화-재생 격리)

---

## 완료 기준 체크리스트

| # | 완료 기준 | 상태 | 근거 |
|---|-----------|------|------|
| 1 | 단위+통합 PASS — remux 재생가능, 크래시 중 kill→재생가능(D7), 재연결 세그먼트 분리 | **PASS** | 단위 138/138, 통합 11/11 (아래 결과) |
| 2 | 채널별 독립 동시 녹화(화면=녹화 일치), 디코딩 원본 스냅샷 PNG | **PASS** | 패킷 fan-out(같은 패킷 디코더·레코더 분배), 화면=녹화 정합 테스트 #149 |
| 3 | 정보바 📷/● 버튼 동작 + 녹화중 표시, 파일 패널 산출물 목록 | **PASS** (M3-5) | `VideoTileWidget`/`FilePanel`, UI 테스트 통과 |
| 4 | 녹화-재생 격리(D8): 녹화 실패가 표시를 멈추지 않음 | **PASS** | `FfmpegRecorder` 무예외 격리, `RecordingController` 시작실패 Idle 유지 |
| 5 | domain/app 순수성 유지(녹화 FFmpeg는 infra만) | **PASS** | `IRecordingSink` 포트가 AVPacket 비노출, FFmpeg는 infra 전용 |
| 6 | 실장비 녹화→재생 검증 | **로컬 대체** | 헤드리스 세션 — 로컬 MediaMTX 종단 검증으로 대체(아래) |

---

## A. onReconnect 세그먼트 배선

**배선 지점:** `src/ui/main.cpp` — 스냅샷 옵저버(control 스레드).

채널 상태 스냅샷 옵저버는 `ChannelController`가 control 스레드에서 발행하므로, 같은
스레드에서 `RecordingController::onReconnect` 호출이 직렬화돼 안전하다. 채널별 직전
`ConnState`를 기억해 **활성 상태(Streaming/SessionOpen 등) → Reconnecting/Stalled
진입 경계(드롭 엣지)** 에서 한 번만 세그먼트를 분리한다.

- `onReconnect`는 내부에서 `splitOnReconnect && state==Recording`인 채널만 처리 →
  **녹화 중이 아닌 채널은 무영향**, 기존 재연결 동작 훼손 없음.
- 분리 시퀀스: `stopRecording`(현 세그먼트 마감 + 소스 `m_recordRequested`/`m_pendingPath`
  클리어) → 새 파일 경로로 `startRecording`. 직전 세그먼트를 덮어쓰지 않는다.
- 소스 레벨(`FfmpegStreamSource::run`)에서도 재open 시 `finishRecorder`로 트레일러를
  써 세그먼트를 마감한다(이중 안전). onReconnect는 **새 경로**를 보장하는 컨트롤러 책임을
  담당한다 — 이게 없으면 재open된 소스가 이전 경로를 재사용해 파일을 덮어쓴다.

검출 전이만 처리하므로 재시도 대기 중(Reconnecting 유지)·재open(Connecting) 반복에서
중복 분리하지 않는다.

---

## B. 통합테스트 결과 — `tests/integration/FfmpegRecorderIT.cpp`

MediaMTX v1.9.3 + ffmpeg publish 하니스(`harness.h` 재사용). `NV_MEDIAMTX_BIN` 가드,
`-j1` 직렬.

```
NV_MEDIAMTX_BIN=$PWD/ops/mediamtx/mediamtx \
  ctest --test-dir build -L integration -j1 --output-on-failure
→ 100% tests passed, 0 failed out of 11 (60.2s)
```

| 테스트 | 검증 | ffprobe 재생가능 | 결과 |
|--------|------|------------------|------|
| #145 녹화 5초→stop | MKV에 video stream + 패킷 존재 | ✅ video stream + 패킷>0 | **PASS** |
| #146 크래시 내성(D7) | stop 없이 소스 강제 종료 후 부분 재생 | ✅ count_packets>0 | **PASS** |
| #147 trailer 누락(외부 ffmpeg SIGKILL) | write_trailer 누락 MKV 패킷 복구 | ✅ count_packets>0 | **PASS** |
| #148 세그먼트 분리 | 재연결 모사(stop→새경로 start) 두 MKV | ✅ 둘 다 video stream + 패킷>0 | **PASS** |
| #149 화면=녹화 정합 | 녹화 패킷수 vs 디코드 프레임수 비율 0.6~1.6 | — (fan-out 정합) | **PASS** |

**D7 핵심:** MKV는 `write_trailer`(Cues/SeekHead) 없이도 `ffprobe -count_packets`로
패킷 복구가 가능하다. #147은 외부 ffmpeg을 SIGKILL해 진짜 trailer 누락을 모사했고,
부분 재생 가능함을 확인했다.

### 픽스처 안정화 (flaky 근본 원인 제거)

기존 픽스처(`make-fixtures.sh` 기본 GOP)는 10초 클립에 **키프레임 1개**(GOP≈300)였다.
RTSP 중간 합류(mid-stream join) 구독자는 다음 키프레임까지 최대 ~10초 대기 →
키프레임 게이트 통과 전이라 `waitFor("packet")`/녹화 세그먼트가 타이밍에 따라 비는
flaky가 있었다(기존 #139 포함 간헐 실패). `-g 30 -keyint_min 30 -sc_threshold 0`
(H.264)/`keyint=30:scenecut=0`(H.265)로 **1초 간격 키프레임(10개/클립)** 을 강제해
중간 합류 대기를 ≤1초로 제한했다. 코덱/해상도/길이 동일(640×480 H.264/HEVC 10s).
픽스처(`*.mkv`)는 gitignore 대상이라 생성 스크립트만 커밋된다 — 재생성 시 안정 픽스처
획득. 단위 138/138 영향 없음(remux 단위 테스트는 전구간 demux라 GOP 무관).

---

## C. 검증 (로컬 MediaMTX 대체)

**실장비 미가용 사유:** 뷰어는 Qt GUI 앱으로 macOS WindowServer(대화형 세션)가 필요하다.
현재 헤드리스 CLI 세션에서는 `open app.bundle`/GUI 토글이 불가하므로, 계획이 허용한
**로컬 MediaMTX 종단 검증**으로 대체한다. 종단 녹화 경로(`FfmpegStreamSource` 패킷
fan-out → `FfmpegRecorder` MKV remux → ffprobe 재생가능)는 통합테스트 #145~#149가
실제 MediaMTX+ffmpeg publish로 전 구간을 커버한다.

### 동시 녹화 부하 (remux 경량성)

4채널 동시 녹화(MediaMTX + 퍼블리셔 4 + 녹화 `-c copy` 4, 뷰어 녹화 경로와 동일한
stream-copy remux):

| 항목 | 측정값 |
|------|--------|
| 녹화 4프로세스 합산 CPU | **~0%** (ps %cpu 샘플링 분해능 이하) |
| 산출물 | load1~4 각 video 패킷 289개, ffprobe 재생가능 |

**해석:** remux는 디코드/인코드 없는 패킷 복사라 CPU가 사실상 0 — 부하는 디스크 쓰기에
지배된다(설계 전제 확인). 채널별 독립 동시 녹화가 CPU 측면에서 가볍다.

### 실장비 스모크 (대화형 세션에서 수행할 절차)

```bash
# 1채널 장비 연결 + 녹화 토글
open build/new_viewer.app --args --connect
#  → 채널 ● 버튼 클릭(녹화 시작, REC 빨강 표시 확인)
#  → 10초 대기
#  → ● 다시 클릭(녹화 중지)
#  → 📷 클릭(스냅샷 PNG)
# 산출물 확인
ls "$HOME/Movies/new_viewer/"
ffprobe -v error -show_entries format=duration -of default=nw=1 \
  "$HOME/Movies/new_viewer/<채널>_<ts>.mkv"   # duration 확인(재생 가능)
open "$HOME/Movies/new_viewer/<채널>_<ts>.png"  # 스냅샷 육안(오버레이 없는 원본)
```

위 절차는 대화형 macOS 세션에서 실행해 결과란을 채울 것. 종단 녹화 로직은 통합테스트로
이미 입증됨 — 실장비는 RTSP 소스/디스플레이 환경 차이만 확인하는 스모크.

---

## D. 판정

| 조건 | 상태 |
|------|------|
| 빌드 성공 | **PASS** |
| 단위 138/138 (`ctest -LE integration`) | **PASS** |
| 통합 11/11 (`-j1`, MediaMTX) — 재생가능·크래시내성·세그먼트·정합 | **PASS** |
| onReconnect 세그먼트 배선(녹화 중만, 기존 동작 보존) | **PASS** |
| 동시 녹화 경량성(remux ~0% CPU) | **PASS** |
| 실장비 녹화→재생 | **로컬 MediaMTX 대체** (대화형 세션 스모크 절차 기재) |

**M3 녹화/스냅샷 — 기능·검증 게이트 PASS.** 실장비 스모크는 대화형 세션에서 절차대로 수행.
