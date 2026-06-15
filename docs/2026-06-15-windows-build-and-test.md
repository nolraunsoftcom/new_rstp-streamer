# Windows 빌드 & 테스트 가이드

**작성:** 2026-06-15. **대상:** new_viewer (main, ui-parity 통합본).
**전제:** 지금까지 전 검증은 macOS에서만 수행. Windows 경로는 `#ifdef`로 **컴파일만 보장**된 상태(부채 #34).
**목표:** 클린 Windows 머신에서 git clone → 빌드 → 단위/통합 테스트 → 30분 통합 스트레스까지 직접 수행.

> 검증 절차(P0~P6) 자체는 `docs/2026-06-15-windows-verification-plan.md` 참고. 이 문서는 **빌드 + 스크립트 실행법**.

---

## 0. 사전 설치 (one-time)

| 도구 | 설치 | 비고 |
|---|---|---|
| **Visual Studio 2022** | Community 이상 + "C++를 사용한 데스크톱 개발" 워크로드 | MSVC v143, Windows 11 SDK |
| **CMake ≥ 3.24** | VS에 포함 / `winget install Kitware.CMake` | PATH 등록 |
| **Ninja** | `winget install Ninja-build.Ninja` | Ninja 생성기 사용 시 |
| **Qt 6.7+** (msvc2022_64) | Qt 온라인 인스톨러 — Widgets/Gui/Network/**ShaderTools** 컴포넌트 포함 | 예: `C:\Qt\6.7.2\msvc2022_64` |
| **vcpkg** | `git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg && C:\dev\vcpkg\bootstrap-vcpkg.bat` | FFmpeg를 매니페스트로 자동 설치 |
| **FFmpeg CLI** | `winget install Gyan.FFmpeg` | `ffmpeg.exe`/`ffprobe.exe` — **테스트 스크립트용**(빌드 FFmpeg와 별개) |
| **mediamtx** | [github 릴리스](https://github.com/bluenviron/mediamtx/releases) `mediamtx.exe`를 PATH에 | v1.19.1 기준 (relay + 합성소스) |
| **adb** | Android Platform-Tools | VOXL2 장비 로그/연결 — PATH 등록 |

> `ffmpeg.exe`, `ffprobe.exe`, `mediamtx.exe`, `adb.exe` 가 **PATH에 있어야** 스크립트가 동작합니다.
> 확인: `where ffmpeg; where ffprobe; where mediamtx; where adb`

---

## 1. 소스 가져오기

```powershell
git clone <repo-url> new_viewer
cd new_viewer
```

vcpkg 매니페스트(`vcpkg.json`)가 FFmpeg(avcodec/avformat/avutil/swscale, GPL-free)를 선언합니다 — configure 시 자동 설치됩니다(첫 빌드는 FFmpeg 컴파일로 수십 분 소요 가능).

---

## 2. 빌드

### 2-1. 편의 스크립트 (권장)

**"x64 Native Tools Command Prompt for VS 2022"** 에서 PowerShell 진입 후:

```powershell
# Ninja (단일 구성, 빠름)
.\ops\win\build.ps1 -QtDir C:\Qt\6.7.2\msvc2022_64 -VcpkgRoot C:\dev\vcpkg

# 또는 Visual Studio 생성기 (다중 구성)
.\ops\win\build.ps1 -QtDir C:\Qt\6.7.2\msvc2022_64 -VcpkgRoot C:\dev\vcpkg -Generator "Visual Studio 17 2022"
```

산출물:
- Ninja: `build\src\new_viewer.exe`
- VS:    `build\src\Release\new_viewer.exe`

windeployqt가 POST_BUILD로 Qt DLL을, vcpkg가 FFmpeg DLL을 exe 옆에 복사합니다.

### 2-2. 수동 CMake (스크립트 없이)

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=C:\dev\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.7.2\msvc2022_64 `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

---

## 3. 단위/통합 테스트 (ctest)

```powershell
# 전체 (통합 테스트 중 일부는 mediamtx/장비 필요 — 환경 없으면 Skipped 또는 Fail)
ctest --test-dir build -C Release --output-on-failure

# 비통합(단위/UI)만 — 환경 의존 없이 깨끗이 통과해야 함
ctest --test-dir build -C Release -E "IT:|IT-|RelayControlApi|LaunchdRelay" --output-on-failure
```

- macOS 기준 비통합 **209/209 통과**. Windows에서도 동일해야 합니다.
- `LaunchdRelay*` 통합 테스트는 macOS 전용(launchd) — Windows에선 자동 제외/Skipped.
- `RelayControlApiIT-B`는 실 mediamtx + publisher 의존(환경/버전 민감) — 단독 실패는 회귀 아님(부채 #32).

---

## 4. 앱 수동 실행

```powershell
# 일반 실행 (GUI)
.\build\src\new_viewer.exe          # (VS면 .\build\src\Release\new_viewer.exe)

# QA 플래그 (스트레스/자동화용)
#   --connect              저장된 채널 자동 연결
#   --auto-record          Streaming 재도달 시 자동 녹화
#   --snapshot-every=N     N초마다 전채널 스냅샷
#   --record-toggle=N      N초 주기로 녹화 토글 (--auto-record와 동시 사용 금지)
.\build\src\new_viewer.exe --connect --auto-record --snapshot-every=30
```

**경로 (Windows):**
| 항목 | 위치 |
|---|---|
| 채널 설정 | `%LOCALAPPDATA%\영상관리시스템\channels.json` |
| relay 설정(앱 생성) | `%LOCALAPPDATA%\영상관리시스템\mediamtx.yml` |
| 녹화/스냅샷 | `%USERPROFILE%\Videos\new_viewer\` (또는 `NV_RECORD_DIR` 환경변수) |

---

## 5. 합성 소스 (장비 없이 멀티채널 부하)

```powershell
.\ops\win\synthetic-source.ps1 start 9      # mediamtx(8654) + cam1..cam9 발행
.\ops\win\synthetic-source.ps1 status
.\ops\win\synthetic-source.ps1 drop cam3    # cam3 중단(독립성/재연결 유발)
.\ops\win\synthetic-source.ps1 restore cam3
.\ops\win\synthetic-source.ps1 stop
```

relay(8554/9997)와 포트가 분리(8654/9998)돼 동시 구동 가능합니다.

---

## 6. 30분 통합 스트레스 (P6)

**장비(VOXL2) USB 연결 + `adb devices`에 1대 보이는 상태**에서:

```powershell
# 30분 (지속600 / churn450 / 장애450 / 마무리300)
.\ops\win\m1m4-stress.ps1

# 1시간
.\ops\win\m1m4-stress.ps1 -Sus 1200 -Chn 900 -Fau 900 -Fin 600

# 장비 URL/합성 채널 수 조정
.\ops\win\m1m4-stress.ps1 -Nsyn 9 -DeviceUrl 'rtsp://169.254.4.1:8900/live'
```

**구성:** 장비 1(relay 경유) + 합성 N(직결) = N+1 채널, 전채널 녹화+스냅샷.
**단계:** ① 지속 → ② viewer churn(껐다켜기 반복) → ③ 장애주입 → ④ 마무리.

**결과:** `logs\m1m4-<timestamp>\summary.txt` (콘솔에도 실시간 출력).

**합격 기준 (macOS 30분 실측 대비):**
| 항목 | 기대 |
|---|---|
| 비정상로그(crash/abort/signal) | **0** |
| 채널 Failed 전이 | **0** |
| churn 중 장비 close 증가 (M4 보호막) | **0** |
| 녹화 손상 | **0** (전부 재생가능) |
| 고유 녹화채널 | **N+1 전채널** |
| teardown 후 잔여 프로세스 | new_viewer/mediamtx/ffmpeg 0 |

> **Windows 차이점:** relay와 합성이 둘 다 `mediamtx.exe`라, 장애주입은 **커맨드라인(설정파일 경로)으로 relay 인스턴스만**
> 종료하고 합성 9채널은 보존합니다. relay 복구는 실행 중 viewer의 `RelayCoordinator` 자가복구(#3)에 의존합니다.
> **클린 `sc stop`/`sc start` 수명관리 검증은 §7 P5에서 별도로** 확인하세요.

---

## 7. Relay 수명관리 (P5) — 수동 확인 포인트

`WindowsRelayService`는 **관리자면 SCM 서비스**(`sc create/start`, `NewViewerRelay`), **비관리자면 schtasks**(`/sc onlogon`) 폴백입니다. 두 권한 모두 확인하세요.

```powershell
# 관리자 PowerShell
sc query NewViewerRelay          # relay 채널 있는 채로 앱 실행 후 → 서비스 설치/RUNNING 확인
tasklist /fi "imagename eq mediamtx.exe"

# 비관리자
schtasks /query /tn NewViewerRelay /v /fo list
```

| 검증 | 기대 |
|---|---|
| **#1 자동 기동** | relay 채널 있는 채로 앱 실행 → mediamtx 자동 기동, 장비 풀(`rtsp://127.0.0.1:8554/dev1` ffprobe h264) |
| **보호막** | viewer 0개여도 mediamtx가 장비 세션 유지(`sourceOnDemand:no`) |
| **#2 닫아도 유지** | viewer 종료 후에도 mediamtx 백그라운드 유지 |
| **재시작 무중단** | viewer 재기동 N회 → 장비 close 증가 **0** (`ensureRunning` 멱등) |
| **#3 셀프힐** | `taskkill /im mediamtx.exe /f` → 실행 중 viewer가 ~9s 내 복구 |

---

## 8. 알려진 갭 / 주시할 것

1. **D3D11 zero-copy 렌더 미구현(스텁)** — macOS는 Metal zero-copy지만 Windows는 D3D11 텍스처↔Qt RHI 공유 경로가 없어 **CPU 왕복(RGBA) 또는 SW 폴백** 가능성. 앱 로그의 `decode path` / `render path` 확인 → **20채널 CPU/드롭프레임에 직결**. 임계 초과 시 별도 구현 작업 등록(부채 #34/갭1).
2. **관리자 권한 정책** — `sc create`는 관리자 필요. 비관리자는 schtasks 폴백으로 자동 전환되나, 표준 배포 정책은 M5 인스톨러와 함께 결정.
3. **mediamtx.exe 경로** — 현재 PATH 의존(`RelayServiceManagerFactory`가 `"mediamtx.exe"` 전달). M5 인스톨러가 고정 경로/레지스트리로 제공해야 함.
4. **SCM 크래시 자동복구** — `sc failure` 미구성(launchd KeepAlive 대비). 셀프힐(#3)이 커버하나 서비스 레벨 복구 추가 검토.

---

## 9. 트러블슈팅

| 증상 | 원인/해결 |
|---|---|
| configure 단계 `Could NOT find FFMPEG` | vcpkg 툴체인 파일 경로 오타 / 첫 빌드 FFmpeg 설치 미완 — 로그 끝까지 대기 |
| `Could not find Qt6` | `-DCMAKE_PREFIX_PATH`가 `...\msvc2022_64`(=`lib\cmake\Qt6` 포함)를 가리키는지 확인 |
| `cl.exe` 못 찾음 (Ninja) | "x64 Native Tools Command Prompt"에서 실행 (VS 환경변수 필요) |
| 실행 시 Qt DLL 누락 | windeployqt 미실행 — 수동: `windeployqt --release build\src\new_viewer.exe` |
| 스크립트 `... cannot be loaded` | 실행 정책: `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass` |
| 한글 채널명 깨짐 | 스크립트는 BOM 없는 UTF-8로 channels.json을 씀(정상). 콘솔 한글은 `chcp 65001` |
| `adb` 장비 안 보임 | USB 디버깅/드라이버 확인, `adb kill-server; adb devices` |
| 스냅샷 0개 수거 | 녹화/스냅샷이 `%USERPROFILE%\Videos\new_viewer\`에 생성되는지 확인 |
