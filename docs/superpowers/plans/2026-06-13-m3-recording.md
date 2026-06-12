# M3: 녹화/스냅샷 (증거 수집) 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 설계 핵심 시나리오 "녹화/증거 수집"을 구현한다 — 정보바 📷/● 버튼 활성, 채널별 독립 동시 녹화(MKV remux, 화면=녹화 일치), 디코딩 원본 프레임 스냅샷(PNG), 우측 파일 탭에 산출물 목록.

**Architecture:** 설계 D3(앱 내 fan-out)·D7(MKV 크래시 내성)·D8(녹화-재생 격리) 준수. 디코드 스레드의 demux 루프가 이미 받는 AVPacket을 **디코더와 레코더 두 소비자에 분배**(av_packet_ref, 디코딩 없는 remux). 녹화는 화면에 보이는 바로 그 패킷을 기록 → 화면=녹화 바이트 일치. 스냅샷은 디코드된 RGBA(오버레이 없는 순수 프레임)를 PNG로. 클린아키텍처: `IRecorder`/`ISnapshotSink` 포트(app), `FfmpegRecorder`/`PngSnapshotWriter`(infra), `RecordingController`/`SnapshotService`(app).

**Tech Stack:** M2 + FFmpeg muxing(avformat output, av_write_frame/av_interleaved_write_frame), Qt(QImage PNG save, QStandardPaths). 녹화는 디코딩 없는 stream copy(remux).

**핵심 결정 (확정):**
- 채널별 독립 동시 녹화(최대 20). remux라 CPU 부담 낮음, 디스크 쓰기가 주부하.
- 스냅샷 = 디코딩 원본 프레임(오버레이 없음, 원본 해상도).
- MKV 컨테이너 + 세그먼트 정책: 재연결 시 새 세그먼트, 주기 롤오버(기본 10분). 키프레임 정렬.
- 1차는 수동 트리거만(버튼). 스케줄/상시 아카이브는 범위 밖.

**브랜치:** `m3-features` (워크트리 `/Users/ieonsang/nolsoft/ziilab/new_viewer-m3`, 이미 생성됨)

---

## 위험·전제 (구현자 필독)

1. **패킷 fan-out 지점**: 현재 `FfmpegStreamSource::run()` demux 루프는 패킷을 디코더에만 보낸다. 레코더 소비자를 추가하려면 루프가 `av_packet_ref`로 패킷을 레코더 큐/싱크에도 전달해야 한다. **디코드 스레드에서 직접 mux 호출은 디스크 I/O 블로킹 위험** → 레코더는 자체 내부 큐+쓰기 스레드를 갖거나, av_interleaved_write_frame이 충분히 빠르면 인라인(remux는 가볍다). 유한 큐 + GOP 단위 드랍(설계: 유인·단기 녹화라 단순 드랍 수용).
2. **remux 타임스탬프**: 입력 패킷의 pts/dts를 출력 stream timebase로 `av_packet_rescale_ts`. 첫 패킷은 키프레임이어야(이미 디코드 경로에 키프레임 게이트 있음 — 레코더도 첫 키프레임부터 시작). 재연결 시 타임스탬프 불연속 → 세그먼트 분리.
3. **녹화-재생 격리(D8)**: 녹화 실패가 재생을 멈추면 안 됨. 레코더 오류는 그 채널 녹화만 중단, 디코드/표시는 무영향. 역도 동일.
4. **스냅샷 프레임 접근**: 디코드된 프레임은 현재 슬롯에 RGBA(동반)로 있다. 스냅샷은 슬롯의 최신 RGBA를 PNG로 저장하면 원본 프레임(오버레이 없음)이 된다. 단 zero-copy 순수 GPU 경로로 가면 RGBA가 빌 수 있으므로(현재는 동반 RGBA 있음) — 스냅샷 요청 시 슬롯에 RGBA가 있으면 그것을, 없으면 GPU에서 1회 readback. M3에서는 동반 RGBA 사용(현 구조).
5. **상태머신 무관**: 녹화/스냅샷은 ConnectionStateMachine과 독립. 녹화 중 끊김→재연결 시 세그먼트만 끊고 재연결 후 새 세그먼트로 이어감.

---

## 파일 구조

```
src/
  app/
    ports/IRecorder.h            녹화 싱크 — start(path,streamParams)/writePacket/stop
    ports/IRecorderFactory.h     채널별 레코더 생성
    RecordingController.h/.cpp    채널별 녹화 수명(start/stop, 세그먼트, 상태)
    SnapshotService.h/.cpp        슬롯 RGBA → PNG 저장 요청 처리
  domain/
    recording/RecordingState.h    Idle/Recording/Stopping + 세그먼트 정책 값
  infra/
    ffmpeg/FfmpegRecorder.h/.cpp   MKV remux (avformat output, 큐+쓰기, 세그먼트 롤오버)
    ffmpeg/FfmpegStreamSource.*    (수정) 패킷 fan-out — 레코더 싱크에도 av_packet_ref
    persist/PngSnapshotWriter.h/.cpp  QImage PNG 저장
    persist/RecordingPaths.h       저장 경로 규칙(QStandardPaths/Movies)
  ui/
    grid/VideoTileWidget.*        (수정) 정보바 📷/● 버튼 활성 + 녹화중 빨강 표시
    panels/FilePanel.h/.cpp       우측 "파일" 탭 — 녹화/스냅샷 목록(썸네일·열기)
    shell/MainWindow.*            (수정) 파일 탭 배선, 녹화 명령 라우팅
tests/
  unit/RecordingControllerTest, SnapshotServiceTest, RecordingStateTest
  integration/FfmpegRecorderIT    (MediaMTX+publish → 녹화 → 재생 가능·세그먼트 검증)
```

---

## Phase 1 — 도메인·포트 (순수)

### Task 1: RecordingState + IRecorder/ISnapshot 포트 + 페이크
- [ ] `src/domain/recording/RecordingState.h`: enum `Idle/Recording/Stopping`, `SegmentPolicy{ maxDuration=10min, splitOnReconnect=true }`. 순수.
- [ ] `src/app/ports/IRecorder.h`: `bool start(const std::string& path, const StreamParams&)`(코덱/timebase 등 remux에 필요한 입력 stream 정보), `void writePacket(const Packet&)`(추상 — 실제론 AVPacket*를 infra가 다룸; 포트는 void*+size 또는 infra 전용 인터페이스로), `void stop()`, `bool isRecording()`. **포트가 AVPacket을 직접 노출하지 않도록** — IRecorder는 infra 경계에 두고 app은 RecordingController가 "녹화 시작/중지"만 명령, 실제 패킷 전달은 infra 내부(FfmpegStreamSource→FfmpegRecorder)에서. 즉 IRecorder는 app 포트가 아니라 **infra 내부 협업**일 수 있음 — 설계 시 app은 "이 채널 녹화 시작" 명령만, 패킷 배선은 infra. 구현자가 이 경계를 명확히 하고 보고서에 기술.
- [ ] 단위테스트: RecordingState 전이.
- [ ] Commit: `feat(domain): RecordingState + 녹화/스냅샷 포트`

---

## Phase 2 — 인프라 (녹화/스냅샷 엔진)

### Task 2: FfmpegRecorder (MKV remux)
- [ ] `FfmpegRecorder`: `start(path, AVStream* inputStream)` — avformat_alloc_output_context2(MKV), avcodec_parameters_copy로 출력 stream 생성, avio_open, write_header. `writePacket(AVPacket*)` — av_packet_rescale_ts + av_interleaved_write_frame. 첫 키프레임부터. `stop()` — av_write_trailer + close. 세그먼트 롤오버(maxDuration 초과 또는 splitOnReconnect)에서 stop+start 새 파일.
- [ ] **격리**: 모든 FFmpeg 오류는 그 레코더만 중단(예외/플래그), 디코드 스레드로 전파 금지.
- [ ] 큐잉: 디코드 스레드 블로킹 방지 — remux가 가벼우면 인라인 허용하되, 유한 큐+GOP 드랍 정책 주석. (측정 후 결정)
- [ ] Commit: `feat(infra): FfmpegRecorder — MKV remux + 세그먼트 롤오버 + 격리`

### Task 3: FfmpegStreamSource 패킷 fan-out + PngSnapshotWriter
- [ ] `FfmpegStreamSource`: demux 루프에서 비디오 패킷을 디코더에 보내기 전 **레코더 싱크가 있으면 av_packet_ref로 전달**. 레코더 시작/중지를 소스에 set(스레드 안전 — 녹화 명령은 control 스레드, demux는 디코드 스레드 → atomic/락). 화면=녹화 일치(같은 패킷).
- [ ] `PngSnapshotWriter`: RGBA(w,h,bytes) → QImage → PNG 저장(경로 규칙).
- [ ] `RecordingPaths`: QStandardPaths(Movies)/앱폴더, 채널명+타임스탬프 파일명.
- [ ] Commit: `feat(infra): 패킷 fan-out(녹화) + PNG 스냅샷 라이터`

---

## Phase 3 — 앱·UI

### Task 4: RecordingController + SnapshotService
- [ ] `RecordingController`(control 스레드): 채널별 녹화 start/stop, FfmpegStreamSource에 레코더 연결, RecordingState 유지, 옵저버로 UI에 녹화중 상태 통지. 재연결 시 세그먼트 분리 처리.
- [ ] `SnapshotService`: 채널의 슬롯 최신 RGBA → PngSnapshotWriter. 비동기(쓰기 스레드 or executor) — UI 블로킹 금지.
- [ ] 단위테스트: Fake 레코더/싱크로 start→writePacket 전달→stop, 재연결 세그먼트 분리, 격리(레코더 실패가 재생 무영향).
- [ ] Commit: `feat(app): RecordingController + SnapshotService`

### Task 5: UI — 정보바 버튼 활성 + 파일 패널
- [ ] `VideoTileWidget` 정보바: 📷(스냅샷)·●(녹화 토글) 버튼 활성화(현재 disabled). 녹화 중이면 ● 빨강 + "REC" 뱃지. 클릭 → control 스레드로 명령 post.
- [ ] `FilePanel`(우측 "파일" 탭, 현재 placeholder): 녹화/스냅샷 디렉토리 스캔, 목록(파일명·크기·시각·썸네일), 더블클릭 열기(QDesktopServices). 새 산출물 시 갱신.
- [ ] MainWindow 배선.
- [ ] Commit: `feat(ui): 정보바 녹화/스냅샷 버튼 + 파일 패널`

---

## Phase 4 — 검증·마무리

### Task 6: 통합테스트 + 실장비 검증 + 문서
- [ ] `FfmpegRecorderIT`: MediaMTX+publish → 녹화 5초 → stop → **MKV 재생 가능 검증(ffprobe), 프레임 수/길이 확인**. + **기록 중 프로세스 kill → 파일 재생 가능**(D7 크래시 내성). + 재연결 주입 → 두 세그먼트 모두 재생 가능.
- [ ] 실장비 스모크: 녹화 시작→영상 지속→중지→재생 확인, 스냅샷 PNG 확인, 화면=녹화 일치 육안.
- [ ] 동시 녹화 부하: sim 다채널 녹화 시 CPU/디스크 측정(remux라 가벼워야).
- [ ] README/tech-debt 갱신(#18 저장실패 알림은 녹화 실패 UI로 일부 흡수 가능 — 검토).
- [ ] Commit: `test(integration): 녹화 재생가능·크래시내성·세그먼트 + M3 문서`

---

## 완료 기준
1. 단위+통합 테스트 PASS — remux 재생가능, 크래시 중 kill→재생가능(D7), 재연결 세그먼트 분리
2. 채널별 독립 동시 녹화(화면=녹화 일치), 디코딩 원본 스냅샷 PNG
3. 정보바 📷/● 버튼 동작 + 녹화중 표시, 파일 패널에 산출물 목록
4. 녹화-재생 격리(D8): 녹화 실패가 표시를 멈추지 않음
5. domain/app 순수성 유지(녹화 FFmpeg는 infra만)
6. 실장비 녹화→재생 검증

## 범위 밖 (M4+)
상시 아카이브 녹화(MediaMTX 내장), 녹화 스케줄, 전체화면 탭(별도 — 설계 §화면구성), 드래그앤드롭 배치, 설정 영속화. (M3 우선순위는 녹화/스냅샷 — UX 완성도/검증마무리는 후속.)
