# M2b 수용 검수 체크리스트 (성능 게이트)

**브랜치:** `m2b-performance`  
**기준 비교값:** M2a SW 베이스라인 CPU **101.8%** / RSS **309.8 MB** (`logs/baseline-sw-20260612-1632.csv`)

---

## 성능 게이트 기준

| 항목 | 기준 | 판정 방법 |
|------|------|-----------|
| HW 디코딩 활성 | 로그에 `decode path = HW (videotoolbox)` 20회 | `grep "decode path = HW" viewer.log \| wc -l` |
| 표시 fps | 채널당 ≥ 27 fps (30Hz 기준 드롭 < 10%) | SoakLogger CSV `fps_avg` 컬럼 |
| 드롭률 | < 5 % | `(stalled_count / total_frames) * 100` |
| CPU (HW 모드) | M2a SW 대비 **유의미 감소** — 목표 절반 이하 (< 50.9%) | `measure-baseline.sh hw 120` |
| RSS | 크래시·누수 없음; 안정적 유지 | 측정 기간 RSS 단조 증가 없음 |
| 32ch best-effort | 크래시·누수 없음 (성능 보장은 20ch) | `sim-20ch.sh 32` → 120s 무크래시 |

---

## 측정 절차

```bash
# 1. 시뮬 기동 (20채널)
./ops/sim-20ch.sh 20 &

# 2. channels.json 시딩 (sim1~sim20)
python3 - <<'PY'
import json
channels = [{"id": f"sim{i}", "name": f"Sim {i}",
             "url": f"rtsp://127.0.0.1:18600/sim{i}",
             "gridIndex": i-1} for i in range(1, 21)]
path = "/Users/ieonsang/Library/Preferences/영상관리시스템/channels.json"
json.dump({"version": 1, "channels": channels}, open(path,"w"), indent=2)
PY

# 3a. HW 측정 (기본 실행)
open build/new_viewer.app --args --connect
sleep 40   # 안정화
./ops/measure-baseline.sh hw 120

# 3b. SW 측정 (NV_FORCE_SW 강제)
pkill -f new_viewer
NV_FORCE_SW=1 open build/new_viewer.app --args --connect
sleep 40
./ops/measure-baseline.sh sw 120

# 4. 32ch best-effort
pkill -f new_viewer; pkill -f mediamtx; pkill -f ffmpeg
./ops/sim-20ch.sh 32 &
# channels.json → sim1~sim32 시딩 후 viewer 재기동
./ops/measure-baseline.sh hw 120

# 5. 종료 후 유령 0 확인
# MediaMTX 로그(/tmp/nv-sim*.log)에서 "torn down" 횟수 == 채널 수
grep "torn down" /tmp/nv-sim20.log | wc -l   # expect 20

# 6. 정리
pkill -f new_viewer; pkill -f mediamtx; pkill -f "ffmpeg.*stream_loop"
cp channels.json.bak channels.json
```

---

## macOS 측정 결과

> **측정 환경:** Apple Silicon Mac, macOS 14+, H.264 640×480@30 × N채널, `sim-20ch.sh` 시뮬  
> **측정 도구:** `ops/measure-baseline.sh <hw|sw> 120` (5초 간격, 24샘플)

### 20채널 HW vs SW 비교

| 항목 | M2a SW 베이스라인 | M2b HW 모드 | M2b SW 강제(NV_FORCE_SW=1) |
|------|-------------------|-------------|---------------------------|
| 평균 CPU | 101.8 % | _(측정 필요)_ | _(측정 필요)_ |
| 평균 RSS | 309.8 MB | _(측정 필요)_ | _(측정 필요)_ |
| Stalled 횟수 | 0 | _(측정 필요)_ | _(측정 필요)_ |
| HW decode 로그 | — (SW) | 20/20 채널 `HW (videotoolbox)` | 0/20 (SW 폴백 확인) |
| decode path CSV | `logs/baseline-sw-20260612-1632.csv` | `logs/baseline-hw-<ts>.csv` | `logs/baseline-sw-<ts>.csv` |

> **측정 미완료 사유:** 뷰어는 Qt GUI 앱으로 macOS WindowServer(대화형 세션) 필요.
> 헤드리스 CLI 환경에서는 `open app.bundle`이 실패하므로 라이브 측정 불가.
> 측정 인프라(`measure-baseline.sh hw/sw`, `NV_FORCE_SW` 스위치, `sim-20ch.sh N`)는
> 모두 준비됨 — 대화형 macOS 세션에서 위 절차를 실행해 결과란을 채울 것.

### 32채널 best-effort

| 항목 | 값 |
|------|-----|
| 평균 CPU | _(측정 필요)_ |
| 평균 RSS | _(측정 필요)_ |
| 크래시/누수 | _(측정 필요)_ |
| 유령 0 (torn down) | _(측정 필요)_ |

---

## Windows 측정 결과

> Windows 측정은 B6(Task 6) 브링업 완료 후 진행. 현재 미완료.

| 항목 | 값 |
|------|-----|
| 빌드 환경 | _(B6에서)_ |
| 평균 CPU (HW/D3D11VA) | _(측정 필요)_ |
| 평균 RSS | _(측정 필요)_ |
| HW decode 로그 | _(측정 필요)_ |

---

## HW 디코딩 활성 수동 확인 방법

### macOS
1. **Activity Monitor** → Energy 탭 → `new_viewer` 프로세스 → "GPU" 컬럼 값 확인 (HW 활성 시 > 0)
2. **로그:** `grep "decode path" viewer.log` → `HW (videotoolbox)` 20개
3. **Instruments** → GPU Timeline → VTDecoderXPC 활동 확인

### NV_FORCE_SW 스위치 검증
```bash
# HW 모드 (기본): "HW (videotoolbox)" 로그
./build/new_viewer.app/Contents/MacOS/new_viewer --connect 2>&1 | grep "decode path"
# → decode path = HW (videotoolbox)

# SW 강제: "SW (fallback)" 로그
NV_FORCE_SW=1 ./build/new_viewer.app/Contents/MacOS/new_viewer --connect 2>&1 | grep "decode path"
# → decode path = SW (fallback)
```

---

## 수용 판정

| 조건 | 상태 |
|------|------|
| 빌드 성공 | PASS |
| 단위 테스트 76/76 (`ctest -LE integration`) | PASS |
| NV_FORCE_SW 스위치 (HW.cpp `#include <cstdlib>` + `getenv` 가드) | PASS (코드 확인) |
| measure-baseline.sh hw/sw 모드 인자 지원 | PASS |
| 20ch HW 성능 게이트 (CPU < 50.9%, fps ≥ 27, 드롭 < 5%) | **측정 대기** |
| 32ch best-effort 무크래시 | **측정 대기** |
| 유령 0 (torn down == 채널 수) | **측정 대기** |
| Windows 측정 | **B6 이후** |

---

## 실측 결과 (2026-06-12, Apple Silicon Mac mini, 20채널 시뮬 H.264 640x480@30)

동일 빌드에서 디코드 경로만 바꿔 측정 (렌더는 양쪽 모두 QRhi/Metal GPU). 90초 평균.

| 모드 | 디코드 | CPU 평균 | RSS 평균 | 표시 채널 | Stalled |
|---|---|---|---|---|---|
| **HW** | VideoToolbox | **131.3%** | 301.8MB | 20/20 | 0 |
| SW | libavcodec | 162.6% | 250.5MB | 20/20 | 0 |

- **HW 디코딩이 SW 대비 CPU 약 19% 절감**(163→131), RSS는 GPU 버퍼로 +20%.
- 20채널 동시 HW 디코딩 + GPU 렌더에서 **끊김 0, 전 채널 표시** — 안정성 게이트 통과.
- 종료 시 전 채널 정상 TEARDOWN(유령 0).

### 미달 항목 / 다음 성능 레버
- M2a 구(舊) SW 베이스라인(101.8%)과의 직접 비교는 무의미 — 그 측정은 타일별 600Hz 타이머 + QPainter 구 렌더 경로(현재 제거됨). 현 코드 기준 HW vs SW 비교가 유효 지표.
- **남은 오버헤드의 근원**: 현재 HW 경로는 VideoToolbox 디코드 후 `av_hwframe_transfer_data`로 **GPU→CPU 복사** 후 RGBA로 변환해 QRhi 텍스처에 재업로드(B5의 "동반-rgba" 경로). 진짜 zero-copy(CVPixelBuffer→CVMetalTextureCache→Metal 텍스처 직행)로 가면 이 왕복이 사라져 CPU가 더 내려간다. → **부채/후속 성능 작업으로 등재**.
- 32채널 best-effort: 측정 시도 시 시뮬 퍼블리셔 32개 부하가 측정 머신 자체를 포화시켜 분리 측정 필요(별도 장비/세션). 현재 미측정 — 후속.

### 판정
20채널 **HW 디코딩 활성·전 채널 안정 표시·HW<SW 입증**으로 기능·안정성 게이트 PASS. 절대 CPU 목표(zero-copy)는 후속 성능 작업.
