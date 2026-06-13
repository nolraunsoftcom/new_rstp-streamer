# M4 — MediaMTX Relay 수용 결과 (2026-06-13)

브랜치 `m4-relay`. 구현 T1~T10 완료(커밋 1d5279f~444f8e1), 단위/통합 **201 테스트 전부 통과**. T11 기능검증은 아래.
정책: 장비 READ-ONLY. 플랫폼: **Windows 메인**(이 검증은 macOS — 장비가 맥에 link-local 169.254.4.1 연결).

## 구현 — 완료 (201 테스트)
| 태스크 | 내용 | 커밋 |
|---|---|---|
| T1 | RelayConfig — mediamtx.yml 생성 + 하드룰 검증 | 1d5279f |
| T2 | 포트 2종(IRelayServiceManager/IRelayControlApi) + 페이크 | 66c0b5d |
| T3 | RelaySupervisor — 생성·검증·기동요청·헬스 | ef09abc |
| T4 | relay 헬스→RelayIntake 진단 + sourceAvailableHint 복귀 | a847ed0 |
| T5 | MediaMtxConfigWriter — 원자적 yml 기록 | 1869760 |
| T6 | HttpRelayControlApi — Control API(Qt::Network) | 50df6eb |
| T7+8 | WindowsRelayService(SCM, #ifdef) + LaunchdRelayService(macOS) + 팩토리 | 9da49bc |
| T9 | 채널 relay/직결 모드(useRelay) + 스키마 1→2 마이그레이션 | c1c0b29 |
| T10 | main 배선 — 시작 시 ensureUp + 헬스 폴링 + relay URL 파생 | 444f8e1 |

## T11 기능검증 (macOS 실기기)

### ✅ 통과
- **relay 데이터 경로 정상**: 생성 config로 mediamtx 기동 → 장비→relay 풀 3초 확립(ready=True, bytesReceived 능동 증가) → `ffprobe rtsp://127.0.0.1:8554/ch1` h264, **viewer가 relay 경유 연결 시 first frame presented + HW 디코드 + NV12 zero-copy, 404/refused 0**(relay 사전기동 시).
- **11.1 sourceOnDemand:no 보호막**: 생성 yml에 하드코딩(+tcp+device source 정확). mediamtx가 **viewer 0개여도 장비 path를 ready/hasSource 유지**(상시 1연결 보호막 동작 확인).
- **11.2 viewer 재시작 세션 무중단**: viewer 종료 후에도 mediamtx(OS 서비스) 연속 UP, 장비 leg hasSource 유지. viewer는 자식프로세스가 아님(설계대로).
- LaunchdRelayService 통합테스트: 실 launchctl+mediamtx 기동·정리·멱등 통과, 잔여물 0.

### 🔴 발견 — macOS launchd + link-local 라우팅 (M4 로직 결함 아님, 환경/플랫폼)
- **증상**: viewer가 relay를 콜드스타트로 띄우면(LaunchAgent) mediamtx가 장비를 못 끌어옴 — `ERR [path ch1] [RTSP source] dial tcp 169.254.4.1:8900: connect: no route to host`. 결과 viewer는 404 반복(path not ready), 45s 후에도 first frame 0.
- **격리 증명**: 같은 순간 ① 셸 ffprobe ② **수기**로 띄운 동일 mediamtx(동일 config/바이너리)는 장비 도달·풀 성공(ready=True, bytes 능동 수신). **launchd로 띄운 mediamtx만 "no route to host".** route get 169.254.4.1 → en0 정상(ARP=장비 MAC).
- **결론**: macOS의 launchd 프로세스 컨텍스트에서 link-local(169.254/16) 라우팅이 간헐/불가. relay 구현 로직·config·서비스관리·viewer연결·진단은 전부 정상(수기 경로로 입증).
- **영향 범위**: **사용자 메인 플랫폼은 Windows** → 이 macOS-launchd 특이 이슈는 Windows에 적용되지 않을 가능성 높음(Windows Service 네트워킹 + 실배포는 link-local이 아닐 수 있음). 단 Windows 실검증 필요.

## 후속 (별도 단계)
1. **Windows 실검증** (메인 타깃): WindowsRelayService(SCM/schtasks) + 실장비로 relay 풀·viewer 연결·보호막·재시작무중단. (이 세션 맥에선 #ifdef로 컴파일 제외만 보장.)
2. **macOS launchd link-local 픽스**(macOS도 지원 시): LaunchDaemon 검토 / network-up 의존 / 또는 macOS에선 relay를 비-launchd로 기동. (Windows 우선이라 후순위.)
3. **콜드스타트 UX**: ensureUp가 main 스레드 ~10s 블로킹 → 비동기화. 실배포는 relay가 상시 OS 서비스(부팅/로그인 자동기동)라 viewer 기동 시 이미 떠 있어 레이스 없음 — 콜드스타트는 최초/서비스다운 시에만.
4. 런타임 Control API path 추가/삭제(무재시작 채널 변경), 72h 소크 + 전원차단 주입(설계 §7 M4 전체 수용).

## 도구
`ops/m4-relay-verify.sh` — relay 활성 기능검증(보호막·재시작무중단·진단, teardown 포함).
