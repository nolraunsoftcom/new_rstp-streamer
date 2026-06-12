# new_viewer 설계 문서

작성일: 2026-06-12
상태: 승인됨 (설계 리뷰 3회 반영 — 최종 2026-06-12)

## 1. 배경과 목표

기존 `viewer`(C++17 + Qt6 + libVLC, ~6,300줄)는 기능은 갖췄으나 작은 결함이 반복 발생하는 구조적 한계에 도달했다. 근본 원인은 네 가지다.

1. 연결 상태머신이 암묵적 — 상태 전이가 `VlcWidget` 곳곳의 플래그로 흩어져 있어 수정이 다른 전이를 깨뜨림 ("가짜 연결" 무한루프가 대표 사례)
2. libVLC가 블랙박스 — 라이브러리 이벤트(`Playing`)와 제품 의미(영상 수신 중)가 달라 워치독 등 우회로만 대응 가능
3. UI와 로직 결합 — 재접속 정책 하나를 검증하려면 위젯을 띄워야 해서 자동 테스트가 불가능 (테스트 0개)
4. 용도별 RTSP 연결 남발 — 재생/녹화/전체화면이 각각 연결을 만들어 장비·relay에 세션이 누적

`new_viewer`는 기능 단위로 검수하며 처음부터 다시 만든다.

### 핵심 운영 시나리오 (우선순위 순)

1. **장시간 관제 안정성** — 장비는 **사람이 휴대하는 카메라**이며 1회 운용 최대 약 12시간. 그 동안 멈춤/누수/좀비 없이 동작, 재접속·복구 완전 자동. 운용 중 장비 전원 차단·배터리 교체 후 복귀는 **정상 흐름**이며 운영자 개입 없이 영상이 다시 떠야 한다 (3차 리뷰: 기존 "드론 며칠 무인 가동" 가정을 실제 운용 형태로 교정)
2. **장애 진단 가시성** — 영상이 안 나올 때 어느 구간(장비/링크/relay/세션/디코딩)이 문제인지 운영자가 즉시 식별
3. **녹화/증거 수집** — 녹화 실패 제로 지향, 크래시에도 그 시점까지의 파일은 생존
4. **규모** — VOXL2 Mini 기반 장비 20대(20채널) 동시 **성능 보장**(게이트 기준선). 20은 하드 한도가 아니며 소프트 한도 32까지 확장 가능 — 21~32채널은 동작 보장 + 성능 best-effort(M2b HW 디코딩 후 재평가) (2026-06-12 사용자 확정)

## 2. 제품 범위 (1차)

### 유지 (기존 기능 정의서 계승)

- 채널 추가/삭제/수정/순서변경, 자동 저장·복원
- 다채널 그리드 재생 (최대 20채널, Auto/1~5 컬럼, 드래그 배치)
- 자동 재접속 + give-up 정책 (명시적 상태머신으로 재구현)
- 녹화(MKV), 스냅샷(PNG), 전체화면 탭
- 로그 패널, 상태바(채널 집계·CPU·메모리·디스크 잔량), 로컬 파일 목록

### 신규

- **단계별 연결 진단**: 채널마다 `장비 도달 → 장비→Relay 수신 → RTSP 세션 → 패킷 수신 → 디코딩 → 표시` 6단계 중 어디서 멈췄는지 표시. 기존의 "가짜 연결"은 버그가 아니라 "세션 열림·패킷 미수신"이라는 정상 진단 상태가 된다.
- **진단 번들 내보내기**: 로그 + 채널 설정 + 버전 + 상태 스냅샷을 zip 하나로 — 현장 이슈 보고 1클릭.

### 제거

- libVLC 의존 전체
- 용도별 다중 RTSP 연결 (앱 내 fan-out으로 대체)

### 보류 (2차 이후)

- 계정/권한, 웹 클라이언트, AI 분석, 알림, 녹화 스케줄
- 상시 아카이브 녹화 (MediaMTX 내장 녹화 활용안 — 1차는 앱 내 트리거 녹화만)
- 다중 PC 동시 시청 (MediaMTX가 이미 수용 가능한 구조, UI/설정만 추가하면 됨)
- 지상측 SSH 자동 복구 훅 (장비 watchdog이 못 잡는 케이스가 현장에서 발견될 때)

## 3. 시스템 구성

```
[VOXL2 Mini × N]
  voxl-streamer (H.264/H.265 — 2026-06-12 장비 공장초기화 후 현재 H.264·UVC 640x480 벤치 구성)
  voxl-streamer-watchdog (장비 내 keepalive + 프로세스/스트림 감시, systemd)
    ※ 반드시 v3(passive) — active probe 버전(v1/v2)은 TCS 폭풍 유발이 실험으로 확정되어 금지
    ※ 2026-06-12 공장초기화로 현재 장비에 미설치. M1 소크 전 `viewer/watchdog/`에서 재배포
        │  RTSP (무선)
        ▼
[관제 PC]
  MediaMTX  ← OS 서비스 (launchd / Windows 서비스). viewer와 생명주기 완전 분리
        │  RTSP (localhost, 채널당 정확히 1연결)
        ▼
  new_viewer (Qt6 + FFmpeg)
        ├─ 그리드 표시 ┐
        ├─ 전체화면 탭 ├─ 앱 내 fan-out (단일 demux에서 분배)
        └─ 녹화(MKV)  ┘
```

### 책임 분리

| 구성요소 | 책임 | 비고 |
|---|---|---|
| 장비 watchdog | 인코더 wedge 원천 차단 (keepalive로 클라이언트 0 방지), voxl-streamer 사망/stall 시 재기동 | 기존 `viewer/watchdog/` 자산(**v3 passive**) 이관. **운영 전제조건으로 프로비저닝 문서에 명시**. 프로비저닝 표준에 **스트리밍 외 서비스(px4·mavlink·vision-hub 등) 비활성** 포함 — 2026-06-12 벤치에서 부하 정리 후 인코더 reopen 실패 0회·TCS Busy 분당 ~1회(이전 수백 회)로 안정성 기여 실측 |
| MediaMTX | 장비당 상시 1연결 유지(지상측 안정 클라이언트), 외부 클라이언트 fan-out(타 PC·VLC 등), Control API로 장비→relay 구간 상태 제공 | 앱 내부 소비자용 fan-out에는 사용하지 않음 |
| new_viewer | 앱 내부 분배(그리드/전체화면/녹화), 상태머신, 진단, 녹화 | MediaMTX에 채널당 연결 1개만 |

### Relay 정책 (필수 규칙)

- `sourceOnDemand: no` — RelaySupervisor의 설정 생성 로직에 **하드코딩**, 기동 시 검증 항목에 포함. on-demand면 보호막이 무효화된다.
- **MediaMTX 버전 고정** — 특정 버전 바이너리 동봉, Control API 스키마 호환성 테스트를 그 버전에 묶음. 업그레이드는 명시적 결정으로만.
- **런타임 채널 변경은 Control API path 단위로만 — MediaMTX 프로세스 재시작 금지** (3차 리뷰 R2). yml 전체 생성은 최초 설치/부팅 시에만. 재시작은 전 장비 세션 동시 단절 = 모든 인코더의 동시 close→reopen churn으로, 이 아키텍처가 막으려는 바로 그 사고를 viewer가 직접 유발하게 된다. **각 채널은 완전 독립** — 한 채널의 추가/삭제/장애가 다른 채널 세션에 영향을 주는 경로가 있어서는 안 된다.
- **RTSP 전송은 양 레그 모두 TCP 강제** (3차 리뷰 R4): 장비 레그는 MediaMTX `rtspTransport: tcp`, 로컬 레그는 FFmpeg `rtsp_transport=tcp` 하드코딩 + 기동 검증 항목. 근거: FFmpeg도 UDP 손실 시 재전송 없이 화면이 깨지고(libVLC 고유 문제가 아님), macOS/Windows 방화벽의 UDP 포트 변수가 현장 설치마다 발목을 잡으며, 이 프로젝트의 모든 안정 동작 검증 이력(ffprobe·ffplay·기존 운영)이 TCP였다. TCP의 head-of-line 지연 트레이드오프는 관제·녹화 용도(초저지연 불요)에서 수용.
- Control API는 `127.0.0.1` 바인딩 — 외부 노출은 RTSP 포트만, 그것도 2차 "다중 PC 시청" 개방 시.
- viewer의 `RelaySupervisor` 역할은 네 가지로 한정: ① 채널 목록 → mediamtx.yml 생성, ② 기동 시 설정 검증, ③ Control API 헬스체크(진단 6단계의 "장비→Relay 수신" 데이터 소스), ④ 미기동 감지 시 OS 서비스 매니저에 기동 요청. **MediaMTX를 자식 프로세스로 띄우지 않는다** (viewer 종료 = 세션 전체 단절 시나리오 방지).
- 직결(직접 RTSP)은 채널 설정 옵션으로 지원 — 벤치 진단·relay 부재 환경용. `IStreamSource`는 URL만 받으므로 아키텍처 비용 없음.

## 4. 기술 스택

| 항목 | 선택 | 근거 |
|---|---|---|
| 언어/표준 | C++20 | |
| GUI | Qt 6 (Widgets), **최소 6.7** | 기존 자산·숙련도, 멀티 비디오 그리드 검증됨. 6.7 하한은 렌더링 경로의 QRhiWidget 요구 (3차 리뷰 R8) |
| 미디어 | FFmpeg (libavformat/libavcodec) 직접 파이프라인 | 프레임 단위 제어 = 진단 가시성, ffprobe로 장비 검증 완료, MediaMTX 상호운용 1순위 조합. VLC는 ModalAI 포럼에서도 고지연·quirk 평가, ModalAI 생태계 선례(QGC)도 "직접 파이프라인" 진영 |
| HW 디코딩 | VideoToolbox (macOS) / D3D11VA (Windows) | 20채널 동시 디코딩 예산 |
| 렌더링 | GPU YUV→RGB (QRhi/OpenGL 셰이더) | 디코딩 1회, 표시 N곳 |
| 빌드 | CMake + vcpkg(FFmpeg, 버전 고정) + Qt 공식 배포판 | |
| 테스트 | Catch2 (또는 GoogleTest) | |
| CI | GitHub Actions, macOS + Windows 매트릭스 | |

### 라이선스 요구사항

- FFmpeg **동적 링크** (LGPL 의무 준수 — vcpkg 동적 트리플렛 사용)
- GPL 컴포넌트(x264 등) **제외 빌드** — 디코딩 전용이므로 불필요. vcpkg ffmpeg 피처에서 x264/x265 제외 + CI에서 빌드 구성 문자열에 `--enable-gpl` 부재 자동 확인
- 위 두 가지를 빌드 문서·CI 검증 항목에 명시

## 5. 아키텍처

### 레이어 (클린아키텍처)

```
Presentation (Qt6)          MainWindow(조립만), ViewModel(QObject, 위젯 없이 테스트 가능)
        ↓
Application (유스케이스)     ChannelManager, RecordingController, SnapshotService,
   포트 인터페이스 정의       DiagnosticsService / IStreamSource, IRecorder, IChannelRepository,
        ↓                    IClock, ILogger, IRelayControl
Domain (순수 C++)           Channel, StreamHealth(6단계), DiagnosisReason,
   Qt/FFmpeg 의존 0          ConnectionStateMachine, ReconnectPolicy, StallDetectionPolicy,
        ↑                    그리드 레이아웃 규칙
Infrastructure (어댑터)      FfmpegStreamSource, FfmpegRecorder, RelaySupervisor 계열,
                            JsonChannelRepository, SystemResourceMonitor, SteadyClock
```

의존 방향: Presentation → Application → Domain ← Infrastructure. Domain은 누구에게도 의존하지 않는다.

### 디렉토리 구조

```
new_viewer/
  src/
    domain/
      channel/      Channel, ChannelConfig
      connection/   ConnectionStateMachine, ReconnectPolicy, StallDetectionPolicy
      health/       StreamHealth(진단 6단계), DiagnosisReason(원인 코드 enum)
      layout/       그리드 Auto 컬럼 규칙, 셀 배치/스왑 로직
    app/
      ports/        IStreamSource, IRecorder, IChannelRepository, IClock, ILogger, IRelayControl
      ChannelManager, RecordingController, SnapshotService, DiagnosticsService
    infra/
      ffmpeg/       FfmpegStreamSource(demux+decode), FfmpegRecorder(remux), FrameConverter
      relay/        RelayConfigWriter, RelayHealthClient(Control API), RelayServiceManager
      persist/      JsonChannelRepository
      system/       SystemResourceMonitor, SteadyClock
    ui/
      shell/        MainWindow (조립 전용)
      grid/         GridView, VideoTileWidget
      channels/     ChannelListView, 채널 다이얼로그
      diagnostics/  DiagnosticsPanel, LogPanel, StatusBar
      viewmodels/   ChannelListVM, TileVM, DiagnosticsVM 등
  tests/
    unit/           domain + app 전체
    integration/    infra/ffmpeg vs 테스트 RTSP 하니스
    fixtures/       H.264 + H.265 샘플 (둘 다 1급)
  ops/              MediaMTX 동봉 바이너리·서비스 등록 스크립트, 장비 프로비저닝 문서(watchdog 포함)
```

### 핵심 설계 결정

**D1. 연결 상태머신은 도메인의 심장.**
`Idle → Connecting → SessionOpen(수신대기) → Streaming → Stalled → Reconnecting → Failed` 전이를 단일 클래스에 명시. 입력은 추상 이벤트 5종(`onSessionOpened / onPacketReceived / onFrameDecoded / onError / onTick`)뿐. 시간은 `IClock` 주입 — fake clock으로 전이 전수 단위테스트.
**`Failed`는 종착역이 아니다** (3차 리뷰 R1): 고속 재접속만 중지하고 **저빈도 재시도 모드(기본 60초 간격)**로 전환하며, `IRelayControl` 헬스체크가 소스 복귀를 감지하면 즉시 재접속을 트리거한다. 근거: 장비는 휴대 카메라로 12시간 운용 중 전원 차단·배터리 교체가 정상 흐름인데, 영구 정지는 그때마다 운영자 개입을 강요해 "복구 완전 자동" 목표와 모순된다. 기존 give-up의 목적(가짜 연결 무한 churn 차단)은 고속 재시도 중지만으로 이미 달성된다.

**D2. 재접속 카운터 리셋 조건은 단 하나 — 프레임 표시 확정.**
세션 열림(SDP 수신)으로는 절대 리셋하지 않는다. 기존 "가짜 연결이 카운터를 리셋해 give-up이 영원히 발동하지 않던" 버그의 교훈을 규칙으로 격상.

**D3. 채널당 RTSP 연결은 정확히 1개, 분배는 앱 내 fan-out.**
단일 demux 루프에서 참조카운트 패킷(`av_packet_ref`)을 디코더 큐·녹화 큐에 분배. 디코딩은 채널당 1회, 그리드/전체화면은 같은 프레임을 GPU에 그리는 것뿐. 효과: 세션 수 = 채널 수(진단 단순), 화면=녹화 데이터 일치(증거성), 디코딩 중복 제거. 소비자 큐는 상호 격리 — 녹화 디스크 지연이 표시를 막지 않는다.
**녹화 큐는 유한(bounded)이며 오버플로 시 패킷을 드랍한다** (3차 리뷰 R3): 녹화는 유인·단기 트리거 녹화(무관제 상시 녹화 아님)라 세그먼트 강제 분할 같은 무거운 대응 대신 단순 드랍을 채택. 단, 임의 지점 드랍은 스트림을 깨뜨리므로 **다음 키프레임까지 GOP 단위로 드랍**해 파일 재생 가능성은 유지하고, 드랍 발생은 `DiagnosisReason::RecorderBackpressure`로 진단 패널에 표면화한다.

**D4. FFmpeg은 어댑터 뒤에 격리.**
`FfmpegStreamSource`의 유일한 책임은 FFmpeg 콜백을 도메인 이벤트 5종으로 번역하는 것. libVLC 시절 "이벤트의 의미" 문제를 좁고 테스트 가능한 번역 계층으로 압축.

**D5. 진단 원인은 `DiagnosisReason` enum으로 코드화.**
`DeviceUnreachable, RelayDown, RelayNoSource, SessionRefused, NoPackets, DecodeError, DiskLow, DiskFull, ...` — UI 문구·로그·테스트가 같은 코드를 공유. 모호한 에러 메시지 원천 차단. `SystemResourceMonitor`는 CPU·메모리에 더해 **녹화 대상 디스크 잔량을 감시**하고, 임계 미달 시 `DiskLow`(경고)/`DiskFull`(녹화 중단 사유)를 발생시켜 상태바·진단 패널에 표시한다 — 장기 가동에서 가장 흔한 녹화 실패 원인의 사전 가시화.
진단 1단계(장비 도달)의 측정은 **ICMP ping 전용, 저빈도(≥10초), 2단계(장비→Relay 수신)가 정상이면 생략** (3차 리뷰 R5). RTSP 포트(8900)에 probe 접속을 만들지 않는다 — 장비측 워치독 v2의 active probe가 인코더를 주기적으로 깨워 TCS 폭풍·wedge를 악화시킨 전례(치료가 병을 악화)의 지상측 재발 방지.

**D6. 스레딩 모델.**
채널당 demux/decode 스레드(FFmpeg 영역) → 프레임 핸들·이벤트를 큐로 전달. **도메인 상태머신(전 채널)은 전용 control 스레드 1개에서만 구동** — 단일 스레드 보장으로 상태 동시성 버그를 차단하는 의도는 유지. UI 스레드 구동안은 기각(3차 리뷰 R6): Windows에서 창 이동/리사이즈 중 modal loop가 QTimer를 수 초씩 정지시켜, 운영자가 창을 끄는 동안 20채널 전체의 stall 감지·재접속 타이머가 멈추는 공백이 생긴다. UI 스레드에는 상태 스냅샷·이벤트만 게시. 렌더링은 GPU.

**D7. 녹화 컨테이너는 MKV (크래시 내성).**
일반 MP4는 종료 시 moov atom 기록 구조라 크래시 시 파일 전체를 잃는다. MKV(또는 필요 시 fragmented MP4)로 기록 중 프로세스가 죽어도 그 시점까지 재생 가능해야 한다. 녹화는 디코딩 없는 remux — 20채널 동시 녹화 CPU ≈ 디스크 쓰기 수준.
**세그먼트 정책**: 녹화 중 demux 재연결이 발생하면 타임스탬프 불연속이 생기므로 기존 파일을 닫고 새 세그먼트로 시작한다. 장시간 녹화의 주기적 분할(예: 10분 단위 롤오버)도 같은 메커니즘으로 처리한다. **새 세그먼트는 키프레임(IDR) 수신 후에만 시작한다** (3차 리뷰 R7) — 키프레임 없이 시작한 파일은 머리가 재생 불능이다. 키프레임 대기 중 버린 구간은 구조화 로그에 기록한다.

**D8. 녹화 실패와 재생은 상호 격리.**
녹화가 죽어도 화면은 유지, 화면 문제가 녹화를 중단시키지 않는다 (공통 원인인 demux 중단 제외).

## 6. 테스트 전략

### 3층 구조

| 층 | 대상 | 방법 | 잡는 버그 유형 |
|---|---|---|---|
| 단위 | domain + app | fake clock + fake 포트. 상태 전이 전수 검증. **기존 버그를 회귀 테스트로 박제**: "SessionOpen에서 5초 패킷 없음 → 카운터 리셋 없이 Reconnecting", "30회 초과 → Failed 전이·고속 재시도 중지·저빈도 재시도(60초) 진입", "Failed 중 relay 헬스 '소스 복귀' → 즉시 재접속" | 가짜연결 루프, give-up 미발동 류 |
| 통합 | infra/ffmpeg | **MediaMTX 바이너리(동봉 고정 버전) + ffmpeg CLI publish**를 테스트 하니스로 — 별도 가짜 서버를 만들지 않고 실전 조합 그대로. 장애 주입: publish 중단(무선 단절), SDP만 주고 패킷 없음(가짜 연결), 기록 중 프로세스 kill(MKV 무결성 — 파일 재생 가능해야 통과), 녹화 중 단절 → 재연결(세그먼트 분할 — 두 세그먼트 모두 재생 가능해야 통과) | 어댑터 번역 오류, 녹화 내성 |
| 수동 QA | 전체 | 기능별 수용 기준 문서(기존 feature-definition 방식 계승) + 진단 패널 자체가 QA 도구. 플랫폼별 체크리스트에 **HW 디코딩 활성 확인**(VideoToolbox/D3D11VA — CI 검증 불가 영역) 포함 | UX, 플랫폼 고유 이슈 |

### 픽스처

- **H.264와 H.265 모두 1급 픽스처.** 장비 운영 인코딩은 시점에 따라 오간다 — 2026-06-12 장비 공장초기화 후 현재는 H.264(voxl-streamer 공장 기본값). 통합 테스트는 두 코덱 모두로 실행.
- **픽스처·시뮬레이션 publish의 해상도/비트레이트는 운영 목표값으로 고정**(목표값 확정 필요): 현 벤치 구성(UVC 640x480@30, 1Mbps)은 운영 대표성이 없으므로 성능 게이트 기준으로 쓰지 않는다 (3차 리뷰).

### 관측성 (이슈 추적)

- 구조화 로그: 모든 상태 전이를 `[채널ID][컴포넌트][전이][DiagnosisReason]`으로 기록, 파일 로테이션
- 진단 번들 내보내기 (§2 신규 기능)
- 소크 테스트 결과물 = 진단 번들 (QA 도구와 운영 도구의 일치)

## 7. 마일스톤과 수용 기준

각 마일스톤은 동작하는 앱을 산출하고, 그 시점의 기능만 검수한다.

### M1 — 1채널 재생 코어
- **선행 조건 변경 (2026-06-12 사용자 결정): 1차 검증은 watchdog 미설치 상태로 진행** — 클라이언트 단독의 견고성(인코더 재오픈 실패 등 장비의 거친 동작 포함)을 먼저 깐깐하게 검증한다. 절차: M1 완료 직후 **2시간 실구동 + 스트레스 테스트**(`ops/stress-m1.md` — 장애 주입 8회, 서버 로그·프레임 전송·재연결 성공률 분석). watchdog(v3 passive) 재배포와 24시간 소크는 2h 검증 통과 후 — 그 단계에서는 원래 논리(watchdog 부재 시 소크 실패 원인에 환경 미비가 섞임)가 그대로 유효하다. 프로비저닝 문서화 자체는 M5.
- FFmpeg 파이프라인 + 상태머신 + 진단 6단계 + 단위/통합 테스트 골격
- 수용 기준: 상태머신 단위테스트 전수 통과. **실장비 직결 24시간 소크** — 메모리 그래프 평탄, 소크 중 발생한 모든 링크 단절(자연 발생 + 인위 주입 최소 3회)에서 수동 개입 없이 자동 회복, 진단 번들 산출
- 산출물: 기준 머신 성능 베이스라인 측정 (M2 게이트 임계값 확정용)

### M2 — 멀티채널 그리드
- 채널 관리/저장/복원, 그리드 레이아웃(Auto 컬럼·드래그 배치)
- 수용 기준: **성능 게이트** — 20채널 동시, HW 디코딩 활성(플랫폼별 수동 확인), CPU/드롭프레임 임계값(M1 베이스라인 기준 확정) 이내, macOS·Windows 양쪽. 20채널 부하는 MediaMTX + ffmpeg publish 시뮬레이션으로 구성(실장비 1대 환경), 스트림 규격은 운영 목표값(§6 픽스처 항목)

### M3 — 녹화·스냅샷·전체화면
- 앱 내 fan-out 완성
- 수용 기준: 기록 중 kill → MKV 재생 가능(통합 테스트), 녹화·재생 상호 격리 시나리오, 화면=녹화 일치 확인

### M4 — MediaMTX 통합
- OS 서비스 등록, RelaySupervisor(설정 생성·검증·헬스체크·기동 요청), Control API → 진단 6단계 통합
- 수용 기준: `sourceOnDemand: no` 검증 테스트, viewer 재시작 시 장비 세션 무중단 확인, **relay 경유 72시간 소크 + 무선 단절 주입** — 자동 복구 및 진단 표시 정확성 확인. 소크 장비는 운영 표준 프로비저닝(워치독 v3 + 불필요 서비스 비활성) + 배터리 전원 등 실제 운용 조건으로 구성. **장비 전원 차단→재투입(배터리 교체 시나리오) 주입 포함** — Failed 저빈도 재시도 경로의 무개입 자동 복귀 검증 (3차 리뷰 R1)

### M5 — 패키징·QA 라운드
- DMG(macdeployqt) / NSIS(windeployqt), MediaMTX 동봉 + 서비스 등록 스크립트, 장비 프로비저닝 문서(watchdog 설치 포함)
- 수용 기준: 클린 머신 설치 → 전 기능 수동 QA 체크리스트 통과, 라이선스 검증(동적 링크·GPL 제외)

## 8. 결정 이력 (요약)

| 결정 | 선택 | 핵심 근거 |
|---|---|---|
| 기능 범위 | 기획 재검토 후 위 §2로 확정 | |
| 미디어 스택 | FFmpeg 직접 파이프라인 (libVLC 폐기) | 진단 가시성, 장비 검증 이력, VLC quirk 전례 |
| MediaMTX | 유지 — 역할 재정의 (장비 보호막 + 외부 배포 + 진단 소스) | voxl-streamer 인코더 재오픈 wedge, viewer 생명주기 분리 |
| 앱 내 fan-out | 채택 — 내부 소비자에 localhost RTSP 사용 안 함 | 세션 수 최소화, 진단 단순화, 화면=녹화 일치 |
| MediaMTX 생명주기 | OS 서비스 (viewer 자식 프로세스 금지) | viewer 종료가 장비 세션을 끊으면 보호막 자기모순 |
| wedge 복구 | 장비측 watchdog (기존 자산 이관, 운영 전제조건) | keepalive가 무선 단절과 무관하게 인코더 보호 |
| 녹화 | 1차는 앱 내 트리거 녹화만, MKV | 상시 아카이브는 2차 보류 |
| 라이선스 | FFmpeg 동적 링크 + GPL 컴포넌트 제외 | 상용 배포 대비 |
| 운용 형태 교정 (3차) | 사람이 휴대하는 카메라, 1회 최대 ~12시간 | 사용자 확인 — "드론 며칠 무인" 가정 폐기 |
| `Failed` 의미 (3차) | 영구 정지 아님 — 저빈도 재시도(60s) + relay 헬스 기반 자동 부활 | 12h 운용 중 전원 재투입이 정상 흐름, 무개입 복귀 필수 |
| 채널 변경 반영 (3차) | Control API path 단위, MediaMTX 재시작 금지, 채널 완전 독립 | 재시작 = 전 인코더 동시 reopen churn 유발 |
| RTSP 전송 (3차) | 양 레그 TCP 강제 | FFmpeg도 UDP 손실에 무방비 + 방화벽 변수 + 검증 이력 전부 TCP |
| 녹화 backpressure (3차) | 유한 큐 + GOP 단위 드랍 + `RecorderBackpressure` 표면화 | 유인·단기 녹화 — 단순 드랍 수용(사용자 결정), 재생성은 GOP 드랍으로 보존 |
| 진단 1단계 (3차) | ICMP 전용·저빈도(≥10s)·2단계 정상 시 생략, RTSP probe 금지 | 워치독 v2 active probe의 TCS 폭풍 전례 |
| 상태머신 스레드 (3차) | 전용 control 스레드 (UI 스레드안 기각) | Windows modal loop 중 QTimer 정지 → 장애 감지 공백 |
| 세그먼트 시작 (3차) | 키프레임(IDR) 정렬 필수 | 비-키프레임 시작 파일은 머리 재생 불능 |
| Qt 하한 (3차) | 6.7 | QRhiWidget 요구 |

## 9. 변경 이력

- **2026-06-12 (3차 리뷰 반영)**: 운용 형태를 "사람이 휴대하는 카메라, 1회 최대 ~12시간"으로 교정(R1)하고 그에 따라 `Failed`를 저빈도 재시도 모드로 재정의. relay 무재시작·채널 독립 규칙(R2), 양 레그 TCP 강제(R4), 녹화 backpressure = GOP 단위 드랍(R3, 사용자 결정), 진단 1단계 ICMP 규칙(R5), 상태머신 전용 control 스레드(R6), 키프레임 정렬 세그먼트(R7), Qt 6.7 하한(R8) 확정.
- **2026-06-12 (장비 현황)**: VOXL2 Mini 공장초기화 수행 — 워치독 제거됨(M1 전 v3 재배포 필요), 현재 H.264·UVC 640x480 벤치 구성, px4 등 비행 서비스 비활성. 부하 정리 후 인코더 reopen 실패 0회·TCS Busy 수백 회→분당 ~1회 실측: 운영 프로비저닝 표준(불필요 서비스 비활성)의 근거.
