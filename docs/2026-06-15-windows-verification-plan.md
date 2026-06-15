# Windows 실검증 계획 (메인 플랫폼)

**작성:** 2026-06-15. **대상:** new_viewer (ui-parity 통합본, 커밋 3c83404 시점).
**전제:** 지금까지 전 검증은 macOS에서만 수행. Windows 경로는 `#ifdef`로 컴파일만 보장된 상태.
**목표:** Windows에서 빌드→기능→성능→relay 수명관리까지 실기기로 검증하고, 미구현/회귀를 식별한다.

---

## 0. 환경 준비

| 항목 | 내용 |
|---|---|
| 빌드 | MSVC(VS 2022) + CMake + Qt6(Widgets/Gui/Network/ShaderTools) + FFmpeg(LGPL 동적). RHI 백엔드 = D3D11. |
| 배포 | `windeployqt`로 Qt 런타임 동봉, FFmpeg DLL 동봉. |
| 장비 | VOXL2 Mini 1대(`rtsp://169.254.4.1:8900/live`) + 합성 RTSP(20ch 부하용, `ops/synthetic-source.sh` 상당의 Windows 구성). |
| mediamtx | `mediamtx.exe` PATH 등록(또는 고정 경로). v1.19.1 기준. |
| 권한 | 관리자/비관리자 2가지 모두 — `sc create`(관리자) vs `schtasks onlogon`(비관리자) 폴백 경로 분기 확인. |

**산출물:** 클린 Windows 머신에서 실행 가능한 빌드 + 실행 절차 문서.

---

## P1. 코어 재생 (1채널 직결)

- [ ] 채널 추가(`rtsp://169.254.4.1:8900/live`) → first frame presented
- [ ] **HW 디코드 경로 확인** — 로그 `decode path = HW (d3d11va)` 기대. **미작동 시 SW 폴백 여부/원인 기록** (HwContext D3D11VA는 현재 스텁 — §알려진 갭 1)
- [ ] **렌더 경로 확인** — `render path = ?`. macOS는 NV12 zero-copy. **Windows zero-copy(D3D11 텍스처→RHI) 미구현 시 RGBA(CPU) 폴백 확인** (§갭 1)
- [ ] 재연결: 장비 끊김→복구 자동 재연결, Failed 저빈도 재시도
- [ ] 30분 단일 채널 안정 — 크래시/누수 0

## P2. 멀티채널 그리드 (M2 성능 게이트 — Windows 필수)

- [ ] 20채널 동시(합성 부하) 재생
- [ ] **CPU/드롭프레임이 M1 베이스라인 임계값 이내** (macOS 기준 20ch CPU ~92%; Windows는 별도 베이스라인)
- [ ] RSS 평탄(누수 0), 1시간 유지
- [ ] **HW 디코드 활성 여부가 성능에 직결** — §갭 1 결과에 따라 게이트 통과/조건부 판정
- [ ] 그리드 컬럼 Auto/수동 전환, 리사이즈 진동 없음

## P3. 녹화·스냅샷·전체화면

- [ ] 녹화 시작→정지 MKV 재생 가능(ffprobe), 강제종료 시 부분재생
- [ ] 스냅샷 PNG 정상(원자적 쓰기)
- [ ] 저장 경로(Windows Movies/AppData) 정상 + 토스트 문구/용량 표기
- [ ] 전체화면 탭(더블클릭) — 영상 출력, 닫기, 녹화 ● 표시
- [ ] 10채널 동시 녹화 1시간 — 손상 0

## P4. 이번 세션 신규/패리티 UI (전수)

- [ ] **DnD**: 채널목록 재정렬(InternalMove), 그리드 타일 이동/swap(드롭 하이라이트)
- [ ] **우클릭 메뉴**: 전체화면/채널정보/수정/스냅샷/녹화/재연결/삭제 + 세퍼레이터
- [ ] **삭제 확인창**: Windows 네이티브 모달(시트는 macOS 전용 — Windows 표시 형태 확인)
- [ ] **채널 정보 다이얼로그**: 정보 표기
- [ ] **토스트**: 한글/액션버튼(폴더/열기/재생/로그) — Windows `QDesktopServices` 폴더 열기 동작
- [ ] **목록 줄바꿈**: 채널목록·파일목록 긴 이름 줄바꿈(가로 스크롤 없음)

## P5. Relay (WindowsRelayService) — 핵심

- [ ] **#1 자동 기동**: relay 채널 있는 채로 실행 → mediamtx 자동 기동
  - 관리자: `sc create start=auto` + `sc start`
  - 비관리자: `schtasks /sc onlogon` + `/run` 폴백
- [ ] **relay 풀**: mediamtx가 장비를 끌어와 `rtsp://127.0.0.1:8554/<id>` 재생(ffprobe h264)
- [ ] **보호막**: viewer 0개여도 mediamtx가 장비 세션 유지(`sourceOnDemand:no`)
- [ ] **#2 닫아도 유지**: viewer 종료 후에도 mediamtx 백그라운드 유지(SCM/schtasks)
- [ ] **viewer 재시작 무중단**: ensureRunning 멱등 → 장비 close 증가 0
- [ ] **#3 셀프힐**: mediamtx 강제 kill(taskkill) → 3폴(~9s) 내 RelayCoordinator가 정리+재기동 감지, 복구. wedge(프로세스 살아있으나 무응답) 모사 시에도 복구
- [ ] **직결↔relay 런타임 전환**: 채널 수정에서 토글 → 재연결, 정보에 모드 반영
- [ ] macOS TCC(로컬네트워크) 관문 **없음** 확인(Windows 이점)

## P6. 통합 스트레스 (1시간)

- [ ] 장비1(relay) + 합성9(직결) 10채널, [지속→viewer churn→장애주입→마무리]
- [ ] viewer 재시작 N회 → 장비 close 증가 0(보호막)
- [ ] relay kill→셀프힐 복구, 합성 채널 독립성, 크래시/누수 0

---

## 알려진 갭 / 결정 필요 (검증 중 확정)

1. **D3D11 zero-copy 렌더 경로 미구현** — `HwContext`의 D3D11VA는 스텁. macOS는 CVPixelBuffer→Metal zero-copy지만 Windows는 D3D11 텍스처↔Qt RHI 공유 경로가 없어 **CPU 왕복(av_hwframe_transfer→RGBA→업로드) 또는 SW 폴백** 가능성. → 20ch 성능에 직결. 검증 결과에 따라 **Windows zero-copy 구현(별도 작업)** 여부 결정.
2. **관리자 권한 정책** — `sc create`는 관리자 필요. 기본 배포에서 관리자 가정할지, schtasks 폴백을 표준으로 둘지 M5 인스톨러와 함께 결정.
3. **mediamtx.exe 경로** — 현재 PATH 의존. M5 인스톨러가 고정 경로/레지스트리로 제공해야 함.
4. **SCM 크래시 자동복구** — `sc failure` 미구성(launchd KeepAlive 대비). 셀프힐(#3)이 커버하나, 서비스 레벨 복구를 추가할지 검토.

---

## 판정 기준

- P1~P4, P6: 기능 동작 + 크래시/누수 0이면 통과.
- P2(성능 게이트): HW 디코드 동작 시 임계 이내면 통과 / 미동작 시 **조건부 통과 + 갭1 후속 작업 등록**.
- P5: #1·#2·#3 + 보호막 + 재시작 무중단 전부 통과해야 relay 수용.
