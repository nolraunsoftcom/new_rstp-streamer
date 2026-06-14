# 1시간 M4 relay 스트레스 — 결과 보고서

**일시:** 2026-06-14 12:37~13:38 (1시간 완주, 2차 시도). 1차(12:20)는 장비 일시 wedge로 중단·진단(아래 §4).
**대상:** m4-relay 빌드(저비용 수정 2건 포함 `4098cea`/`9142923`), relay 모드(useRelay=true), 로컬네트워크 권한 승인됨.
**구성:** [지속 20m]→[viewer churn 15m, relay 유지]→[장애주입 15m, relay 중단/재개]→[마무리 10m]. caffeinate.
**로그:** `logs/m4stress-20260614-1237/`

---

## 결론: 기능 PASS + 🔴 보호막 버그 1건 발견(근본원인 규명·수정안 확정)

1h 능동 스트레스를 크래시 0·누수 0·녹화/스냅샷 무손상으로 완주했고, **relay 보호막이 viewer 재시작마다 깨지는 버그**를 잡아냈다(근본원인 = 서비스 매니저의 비멱등 재시작). 수정 명확.

## 1. 안정성/기능 — PASS
| 지표 | 결과 |
|---|---|
| viewer 기동 | 13회 / **비정상종료 0** |
| first frame presented | 14 (전 기동 relay 경유 연결) |
| 상태 | Stalled 1 / Reconnecting 52 / **Failed 0** (전부 복구) |
| **녹화** | **24 재생가능 / 손상·빈 0** — Fix A(헤더-only 제거) 실동작 검증(churn 강제종료 다수에도 0) |
| 스냅샷 | **109 정상 / 0 손상** |
| **RelayDown 진단** | **104회** — Fix B(relay connect거부→RelayDown) 실동작 검증(장애주입 구간) |
| relay 다운→복구 | 중단(UP=n)→재개(UP=y, hasSource=yes) 자동 ✅ |
| RSS | 전반¼ 159 ↔ 후반¼ 163MB (58샘플, 재시작 가로질러) — 누수 없음 |

## 2. 🔴 핵심 발견 — relay 보호막이 viewer 재시작마다 깨짐

**증상:** churn 구간 viewer 재시작 10회 → **장비 소스파이프 close/reopen 정확히 +10** (1:1). 직결 8h(81회)와 동일 패턴. 설계 의도(relay가 장비 상시연결 유지 → viewer 재시작이 장비에 무영향)와 정반대.

**근본원인 규명(배제법):**
- ❌ URL 버그 아님 — `lsof` 확인: viewer→`127.0.0.1:8554`(relay), relay→`169.254.4.1:8900`(장비). viewer는 장비에 직접 안 붙음.
- ❌ mediamtx 소스드롭 아님 — ffmpeg reader 연결/종료 테스트: reader 0이 돼도 mediamtx가 장비 소스 유지(bytesRecv 계속 증가, ready=True). `sourceOnDemand:no` 정상 동작.
- ✅ **진짜 원인: `LaunchdRelayService::ensureRunning`이 비멱등** — viewer가 **매 기동마다** `ensureUp`→`ensureRunning` 호출, 그 안에서 **이미 떠있는 relay도 `launchctl bootout`(중단)→재시작**. relay 재시작 → 장비 재-풀 → 장비 close/reopen. **viewer 재시작 N회 = relay 재시작 N회 = 장비 close N회.**

**수정안(확정):** `ensureRunning`을 진짜 멱등으로 — **이미 실행 중 + 설정(configPath) 동일이면 bootout/재시작 없이 즉시 return**. 미실행이거나 설정 변경 시에만 (재)기동. WindowsRelayService도 동일 적용. 수정 후 churn 시 장비 close ≈ 0이어야 보호막 입증.

## 3. (저심각도) 콜드스타트 초기 stall
첫 기동(콜드)에서 relay+장비풀 확립 전 수~수십초 stall→Connecting 반복 후 정착. 실배포는 relay 상시서비스라 콜드스타트 드묾. ensureUp 비동기화로 개선 가능(별도).

## 4. 1차 시도 중단 — 장비 일시 wedge (relay 무관, 기록)
1차(12:20)에서 viewer가 ~50초씩 끊김 반복. 진단 결과 **장비(voxl-streamer) 인코더 일시 wedge**(gbm_create_device 재init 반복, bytesRecv 정지)였음 — relay/viewer 무관. 중단·휴식 후 직결 90초(2615frame 연속)·relay 90초(bytesRecv 연속) 둘 다 정상 확인 → 장비 회복. 하루 종일 누적 부하(8h 소크+M4 다수)로 인한 일시 wedge로 추정. **시사: relay는 클라이언트-churn 인한 장비 close/reopen은 막지만(위 버그 수정 시), 장비 인코더 자체 wedge는 못 막음 → 장비측 watchdog(v3 passive, 보류중) 필요.**

## 5. 다음
1. **[필수] `ensureRunning` 멱등 수정** + churn 재검증(장비 close ≈0 확인) — 보호막 핵심.
2. WindowsRelayService 동일 멱등 + Windows 실검증(메인 타깃).
3. (저심) 콜드스타트 ensureUp 비동기화.
4. 장비측 watchdog 재배포(인코더 wedge 대비), 72h 소크.
