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

### ✅ 해결됨 — macOS Local Network Privacy(TCC) (2026-06-14)
- **초기 증상**: viewer가 relay를 콜드스타트로 띄우면(LaunchAgent) mediamtx가 장비를 못 끌어옴 — `ERR [path ch1] [RTSP source] dial tcp 169.254.4.1:8900: connect: no route to host`. viewer는 404 반복.
- **근본 원인 규명**: 격리 실험으로 launchd 전체가 아닌 **바이너리별** 문제 확인 — launchd python(Apple 서명)은 link-local 도달 OK, launchd mediamtx(adhoc 서명)만 실패, 셸에서 띄운 mediamtx는 OK. → **macOS 26 Local Network Privacy(TCC)**: 셸 실행은 Terminal(권한 보유)의 responsible-process를 상속해 허용, launchd 실행은 mediamtx 자신이 responsible-process라 미허가 → 로컬네트워크(link-local) 아웃바운드 거부.
- **해결**: 사용자가 시스템 설정에서 **로컬 네트워크 권한 승인**. 승인 후 launchd mediamtx가 장비 풀 성공(ready=True, bytes 164K→712K 능동 수신). **전체 verify PASS**: 콜드스타트로 viewer가 relay ensureUp→장비 풀→relay 경유 first frame presented(HW+zero-copy), 보호막(0 viewer에도 hasSource), viewer 재시작 세션 무중단 전부 확인. (이전의 "콜드스타트 레이스" 실체 = 권한 차단으로 풀이 영영 실패한 것.)
- **배포 시사**: macOS 정식 배포는 M5에서 Developer ID 서명 + 로컬네트워크 권한을 설치 흐름에 포함해야 사용자 1회 승인으로 자동화됨. **Windows(메인)은 이 TCC 관문 없음** → 별도 실검증만 필요.

## 후속 (별도 단계)
1. **Windows 실검증** (메인 타깃): WindowsRelayService(SCM/schtasks) + 실장비로 relay 풀·viewer 연결·보호막·재시작무중단. (이 세션 맥에선 #ifdef로 컴파일 제외만 보장.)
2. **macOS 정식 배포(M5)**: Developer ID 서명 + 로컬네트워크 권한을 설치 흐름에 포함 → 사용자 1회 승인으로 자동화. (현재는 사용자가 수동 승인해 동작 확인 완료.)
3. **콜드스타트 UX**: ensureUp가 main 스레드 ~10s 블로킹 → 비동기화. 실배포는 relay가 상시 OS 서비스(부팅/로그인 자동기동)라 viewer 기동 시 이미 떠 있어 레이스 없음.
4. 런타임 Control API path 추가/삭제(무재시작 채널 변경), 72h 소크 + 전원차단 주입(설계 §7 M4 전체 수용).

## 도구
`ops/m4-relay-verify.sh` — relay 활성 기능검증(보호막·재시작무중단·진단, teardown 포함).

---

## M4 코드 리뷰 대응 (2026-06-14)

| # | 지적 | 처리 | 커밋 |
|---|---|---|---|
| 1 | FfmpegRecorder 미커밋 | stale — 이미 커밋됨(트리 클린) | `4098cea` |
| 2 | auto.key/crt untracked, .gitignore 없음 | .gitignore에 `auto.*`/`*.key`/`*.crt` 추가 + 키 제거(과거 이력 없음 확인) | `1ad9e53` |
| 3 | RelayConfig YAML 주입(이스케이프/runOn 차단 없음) | generate가 source를 YAML 이중따옴표 이스케이프(\,",제어문자 제거), validate가 runOn* 명령키 블록리스트 거부. **악성 URL 개행주입 차단 실증** | `27aaea4` |
| 4 | pollHealth UI스레드 동기HTTP + ensureUp 블로킹 | RelayCoordinator(전용 QThread)로 ensureUp+폴링 이동, 헬스결과는 control 스레드로 마샬. UI 무블로킹. clean teardown(shutdown→quit→wait). **relay 워커스레드 실작동+clean exit 검증, 보호막 무회귀(churn close +0)** | `5c9d298` |
| 5 | URL 검증 약함(startsWith만) | ChannelDialog: QUrl(StrictMode)+isValid+host 비어있지않음+제어문자 거부(레거시 패리티) | `27aaea4` |
| 6 | 런타임 채널변경 relay 미반영 | RelayCoordinator.updateChannels 훅 배선(채널변경→config 재생성, ensureUp 멱등) | `5c9d298` |

### 남은 Low(후속)
- LaunchdRelayServiceIT teardown이 `/tmp/nv_launchd_it_relay.yml`·로그를 남김(테스트 위생). 제품 무관, IT 정리 보강 권장.
- plist/sc 명령은 현재 입력이 전부 앱 상수(HOME·고정 label·cfgDir)라 안전 — 사용자 유래 입력 추가 시 XML/셸 이스케이프 필요. 불변량 주석 권장.
- 콜드스타트 first-frame ~28s(relay 부팅 16s + viewer 재연결) — 실배포는 상시서비스라 즉시. 워커스레드화로 UI 블로킹은 제거됨.
