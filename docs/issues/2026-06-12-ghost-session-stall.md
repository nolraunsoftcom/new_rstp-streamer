# 이슈: 유령 RTSP 세션 청소 시점에 현재 스트림이 멈춤 (직결, voxl-streamer)

상태: **원인 확정** (장비 소스코드 분석으로 H-A 검증 완료) + 우리측 완화 배포 완료
발견: 2026-06-12 1시간 스트림 테스트 (`logs/` 1032 세트)
영향: 직결 모드에서 ~75초 주기 스트림 단절 반복 (viewer는 매회 무개입 자동 복구 — 클라이언트 영향은 가용성 저하뿐)

## 증상

- 연결 후 ~55-60초에 패킷 중단 → viewer stall 감지(10s) → 재접속(+9s) → 다시 ~60초 후 중단. 1h 테스트에서 15회.
- 동일 코드·동일 조건에서 **40분 연속 무결점 구간 존재** (10:44~11:23) — 결정적 모순이었음.
- 별개 증상: 간헐적으로 장비가 신규 RTSP 연결 자체를 거부 (순정 ffmpeg CLI도 open timeout — 클라이언트 무관 확정).

## 핵심 증거: Removed = 유령 세션 청소 (+60초 정각 상관)

장비 로그의 `Removed 1 sessions` 17건을 전수 대조한 결과, 모두 **직전 클라이언트 disconnect + 60(±5)초** 시점에 발생:

| 직전 disconnect | Removed | Δ |
|---|---|---|
| 21:12:54 | 21:13:58 | +64s |
| 21:14:09 | 21:15:14 | +65s |
| 21:15:28 | 21:16:28 | **+60s 정각** |
| 21:16:40 | 21:17:40 | **+60s 정각** |
| 21:17:58 | 21:19:02 | +64s |
| 21:19:13 | 21:20:16 | +63s |
| 21:20:35 | 21:21:40 | +65s |
| 21:21:50 | 21:22:56 | +66s |
| 21:23:07 | 21:24:10 | +63s |

gst-rtsp-server의 세션 풀 청소(기본 타임아웃 60s)가 **TEARDOWN 없이 끊긴 직전 세션의 유령**을 제거하는 것. 그리고 매번 그 직후 1~9초 안에 현재 스트림의 패킷이 중단됨.

## 가설

**H-A (유력): 유령 세션 청소의 부작용이 현재 송출을 멈춘다.**
- 청소 → 스트림 멈춤 → viewer가 stall로 세션 종료(이 종료가 또 유령을 남기면) → 재접속 → 60초 후 또 청소... **자기영속 루프**.
- 40분 클린 구간 설명: 어느 사이클에서 TEARDOWN이 깨끗이 성공 → 유령 없음 → 청소 없음 → 안정 지속. 클린 구간 40분간 장비 이벤트 0건이 방증.
- 최초 트리거 설명: 캐스케이드 시작 직전에 구 viewer를 `pkill`(SIGTERM, TEARDOWN 없이 강제종료)한 이력 있음 = 최초의 유령.
- 클린 구간의 끝(22:04:21)은 Removed 없이 disconnect가 먼저 → 이건 유령과 무관한 **장비 파이프라인 자체 멈춤**(별개 증상)이 트리거였고, 그 후 루프 재진입.

**H-B (반증 필요): 현재 세션의 keep-alive 미갱신으로 인한 자체 타임아웃.**
- +60초 상관이 "직전 disconnect" 기준으로 정렬되고 "현재 connect" 기준이 아니므로 H-A보다 약함. 단, 패킷 캡처로만 최종 배제 가능.

## 검증 실험 계획

**E1. 유령-멈춤 인과 직접 검증 (가장 결정적, 회당 ~2분, 장비 무변경)**
viewer가 건강하게 스트리밍 중일 때: ffprobe로 2번째 세션 접속 → 10초 후 `kill -9`(고의 유령 생성) → 60~70초 후 장비 로그의 Removed 시점에 viewer 스트림이 멈추는지 관찰. 멈추면 H-A 확정. 3회 반복.

**E2. TEARDOWN 송신 감사 (tcpdump)**
`sudo tcpdump -i <if> -w rtsp.pcap host 169.254.4.1 and port 8900` 켜고 ① 정상 해제 버튼 ② stall 후 자동 재접속 ③ 앱 강제종료 — 각 경로에서 TEARDOWN이 실제 송신되는지. (우리 close 경로는 interrupt 후 `avformat_close_input`인데, 끊긴 소켓에서 TEARDOWN이 유실될 수 있음)

**E3. keep-alive 감사 (E2와 같은 캡처에서)**
GET_PARAMETER/OPTIONS가 ~30초 주기로 나가는지 + SETUP 응답의 `Session: ...;timeout=` 값 확인. H-B 배제용.

**E4. 신규 연결 거부("뻗음") 관찰**
발생 시 즉시: `adb shell "netstat -tn | grep 8900"`, `adb shell top -n1 | head -15`, voxl-uvc-server 로그 — 서버측 소켓/부하 상태 채증.

## 대응 방향 (원인별)

- **H-A 확정 시**: ① (우리측 완화) 종료 경로에서 TEARDOWN 보장 강화 — SIGTERM/SIGINT 핸들러로 graceful shutdown 추가(제품 위생상 무조건 가치 있음), stall-close 시 TEARDOWN 유실 여부 보완. 단 무선 단절 등 유령이 불가피한 경우는 남으므로 ② (장비측 근본) ModalAI 포럼/이슈 보고 대상. ③ (아키텍처) M4의 MediaMTX 경유가 기본이 되면 장비 레그는 상시 1연결로 유령 자체가 안 생김 — **이 이슈는 M4 아키텍처로 구조적으로 흡수됨**. 직결은 진단용이므로 문서화로 충분할 수 있음.
- **H-B 확정 시**: FFmpeg keep-alive 옵션/버전 검토, 또는 동일하게 M4로 흡수.
- **신규 연결 거부 증상**: E4 채증 후 별도 판단 (장비 재부팅/프로비저닝 정리로 해소되는지 포함).

## 원인 확정 (2026-06-12 오후, voxl-streamer 소스 분석)

voxl-streamer `main.c`의 `timeout()` 콜백(2초 주기)이 범인이다:

```c
guint removed = gst_rtsp_session_pool_cleanup(pool);
if (removed > 0){
    ctx->num_rtsp_clients--;          // ← 무조건 감소: 유령 청소인데 산 클라이언트 수를 깎음
    if(ctx->num_rtsp_clients == 0){   // ← 0이 되면 프레임 카운터·need_data 리셋
        ... // = 소스 파이프 닫힘 → 살아있는 클라이언트의 스트림 중단
```

- 정상 disconnect 핸들러(`rtsp_client_disconnected`)도 감소시키므로, 유령 1개가 청소되면 **더블 디크리먼트** → 산 클라이언트 1명이 있어도 카운트 0 → 송출 중단. 우리가 관찰한 자기영속 루프와 정확히 일치.
- 역사: forum 스레드 #1647의 teardown-request 미처리 문제를 고치려고 **v0.4.2에 커뮤니티 PR로 들어온 코드가 이 버그를 도입**, 최신 sdk-1.7.0(2026-03)까지 미수정.
- 동일 증상의 기존 보고는 포럼에 없음 → 우리가 최초 보고자가 될 수 있고, 코드 수준 분석 + 재현 절차를 보유.
- 상세: `.omc/research/modalai-ghost-session.md` (출처 URL·코드 인용 전체)

## 우리측 완화 (배포 완료, fd0afca)

1. close 경로 TEARDOWN 보장 — 2단계 interrupt (읽기 즉시 중단 → close_input 1초 유예로 TEARDOWN 송신)
2. SIGTERM/SIGINT graceful shutdown (self-pipe + QSocketNotifier)
3. 회귀 방지: 통합테스트 "close()는 TEARDOWN을 송신한다" 추가 — MediaMTX 로그의 `destroyed: torn down by`로 검증 (장비 상태와 무관한 재현 환경)

한계: 무선 단절·전원차단처럼 TEARDOWN 기회 자체가 없는 경우의 유령은 막을 수 없음 → 장비측 수정(보고) 또는 M4 흡수가 필요.

## 참고

- viewer 평가: 이 이슈 동안 15/15 무개입 자동 복구, 진단 코드 일관(NoPackets), 자원 누수 없음 — 클라이언트 견고성 목표는 충족. 패킷 흐름 실시간 표시(`82bd1a8`)로 이후 관찰은 즉시 가시화됨.
- 데이터: `logs/viewer-1h-20260612-1032.log`, `logs/device-1h-20260612-1032.log`, `logs/csv-1h-20260612-1032.csv`, `logs/netstat-1h-20260612-1032.log`
