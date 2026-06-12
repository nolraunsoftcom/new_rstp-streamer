# M2b: 성능 경로 (HW 디코딩 + GPU 렌더 + Windows + 성능 게이트)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** M2a의 기능을 유지한 채 디코딩·렌더 엔진을 교체해 20채널 성능 게이트를 통과하고(macOS+Windows), 31~32채널 best-effort 여력을 확보한다. M2a 추가 리뷰가 짚은 M2b 선행 3건(프레임 서피스 포트, 단일 렌더 타이머, 스키마 버전)을 먼저 깐다.

**Architecture:** 디코드 경로를 SW(sws_scale→RGBA vector) → HW(VideoToolbox/D3D11VA → GPU 텍스처)로 교체하되, 어댑터 경계 뒤에서만 바뀌게 한다. 프레임 전달을 `IFrameSurfaceRegistry` 포트로 추상화해 RGBA vector와 GPU 텍스처가 같은 인터페이스로 흐르게 하고, 타일별 30Hz 타이머(20ch=600Hz)를 **단일 vsync 동기 repaint 타이머**로 대체한다. 렌더는 QRhiWidget(YUV/텍스처→화면).

**Tech Stack:** M2a + FFmpeg hwaccel(av_hwdevice_ctx_create, AV_PIX_FMT_VIDEOTOOLBOX/D3D11), Qt6.7 QRhi(QRhiWidget), vcpkg(Windows FFmpeg 동적·GPL제외). 기준 부하: H.264 640x480@30 × 20채널. **베이스라인 비교 기준: M2a SW 측정값 CPU 101.8% / RSS 310MB** (`logs/baseline-sw-*.csv`).

**브랜치:** `m2b-performance` (main에서 분기 — M2a 머지 후)

**선행 가정:** M2a가 main에 머지됨. 미머지면 m2a-multichannel에서 분기.

---

## 파일 구조 (신규/변경)

```
src/
  app/ports/
    IFrameSurface.h            프레임 1장 추상 (CPU RGBA | GPU 텍스처 핸들)
    IFrameSurfaceRegistry.h    채널ID→최신 서피스 조회 포트 (ChannelSourceFactory가 구현)
    IVideoRenderer.h           서피스를 위젯에 그리는 포트
  infra/
    video/FrameSurface.h       IFrameSurface 구현 (CPU/HW 두 변종)
    video/LatestSurfaceSlot.h  LatestFrameSlot 후속 — 서피스 보관 (RGBA or VideoToolbox CVPixelBuffer ref)
    ffmpeg/HwContext.h/.cpp     hwdevice 생성·get_format 콜백·픽셀포맷 협상
    ffmpeg/FfmpegStreamSource.* (수정) HW 디코드 경로 + SW 폴백
    persist/JsonChannelRepository.* (수정) {"version":1,"channels":[...]} envelope
  ui/
    render/RhiVideoRenderer.h/.cpp  QRhiWidget 기반 GPU 렌더러 (IVideoRenderer 구현)
    render/SwVideoRenderer.h/.cpp   QPainter 폴백 (현 VideoTileWidget 로직 이관)
    grid/VideoTileWidget.*      (수정) 자체 타이머 제거 → 외부 repaint 신호 구독
    shell/RepaintClock.h        단일 vsync repaint 타이머 (전 타일 1회/프레임 갱신)
    main.cpp                    (수정) SoakLogger 추출, 렌더러/레지스트리 배선
ops/
  measure-baseline.sh          (수정) HW 모드 측정 — sw csv와 같은 포맷으로 hw csv
  m2b-acceptance.md            성능 게이트 체크리스트
.github/workflows/ci.yml       macOS + Windows 매트릭스 빌드+테스트
vcpkg.json                     FFmpeg 동적·GPL제외 매니페스트
```

---

## Phase 1 — 아키텍처 선행 (M2b 코드 줄어들기 전에 깔기)

### Task 1: IFrameSurface + Registry 포트 + 스키마 버전 envelope

**Files:** Create `src/app/ports/IFrameSurface.h`, `src/app/ports/IFrameSurfaceRegistry.h`; Modify `src/infra/persist/JsonChannelRepository.cpp`, Test `tests/unit/JsonChannelRepositoryTest.cpp`

- [ ] **Step 1: `src/app/ports/IFrameSurface.h`** — 프레임 1장 추상:

```cpp
#pragma once
#include <cstdint>
#include <vector>

namespace nv::app {

// 디코드 결과 1장. CPU 변종(RGBA tight)과 GPU 변종(플랫폼 텍스처 핸들)을 같은 타입으로 흐르게 한다.
// M2b: CPU 변종은 SW 폴백/테스트용, GPU 변종이 기본 경로.
struct FrameSurface {
    enum class Kind { None, CpuRgba, GpuTexture };
    Kind kind = Kind::None;
    int width = 0;
    int height = 0;
    uint64_t seq = 0;

    // CpuRgba일 때만 유효
    std::vector<uint8_t> rgba;          // tight, stride = width*4

    // GpuTexture일 때만 유효 — 불투명 핸들 (VideoToolbox: CVPixelBufferRef, D3D11: ID3D11Texture2D*).
    // 수명: 레지스트리가 ref를 보유, 소비자는 그릴 동안만 유효. void*로 두고 인프라 계층에서 캐스팅.
    void* gpuHandle = nullptr;
};

} // namespace nv::app
```

- [ ] **Step 2: `src/app/ports/IFrameSurfaceRegistry.h`** — UI가 채널ID로 최신 서피스를 조회:

```cpp
#pragma once
#include <string>
#include "IFrameSurface.h"

namespace nv::app {

class IFrameSurfaceRegistry {
public:
    virtual ~IFrameSurfaceRegistry() = default;
    // lastSeq보다 새 프레임이 있으면 out에 채우고 true. CPU/GPU 변종 모두 지원.
    virtual bool latestSurface(const std::string& channelId, FrameSurface& out,
                               uint64_t lastSeq) = 0;
};

} // namespace nv::app
```

(ChannelSourceFactory가 Task 4에서 이 인터페이스를 구현. GridView는 ChannelSourceFactory 구체타입 대신 이 포트에 의존 → M2a 부채 #6 해소.)

- [ ] **Step 3: 스키마 버전 envelope** — `JsonChannelRepository`:
  - save: `{"version":1,"channels":[...]}` 형태로 기록.
  - load: 최상위가 객체이고 "channels" 배열이면 그것을, **최상위가 배열이면(구버전) 그대로** 읽음(하위호환). 둘 다 아니면 빈 목록.

테스트 추가:
```cpp
TEST_CASE("envelope 형식 저장→로드 라운드트립") { /* version 키 포함 저장, 재로드 일치 */ }
TEST_CASE("구버전 최상위 배열도 로드된다(하위호환)") { /* "[{...}]" 파일 → 정상 로드 */ }
TEST_CASE("version 키 누락 객체도 channels 있으면 로드") {}
```

- [ ] **Step 4:** 빌드+테스트 PASS → Commit: `feat(app): IFrameSurface/Registry 포트 + channels.json 버전 envelope`

---

### Task 2: 단일 repaint 타이머 (RepaintClock) + VideoTileWidget 타이머 제거

**Files:** Create `src/ui/shell/RepaintClock.h`; Modify `src/ui/grid/VideoTileWidget.h/.cpp`, `src/ui/main.cpp`

- [ ] **Step 1: `src/ui/shell/RepaintClock.h`** — 전체 1개 타이머:

```cpp
#pragma once
#include <QObject>
#include <QTimer>

namespace nv::ui {

// 앱 전체 단일 repaint 펄스 (~30Hz). 타일마다 타이머를 두지 않는다 (20ch×30Hz=600Hz 방지).
// M2b: 가능하면 QRhiWidget의 frameReady/vsync에 묶고, 폴백으로 QTimer 33ms.
class RepaintClock : public QObject {
    Q_OBJECT
public:
    explicit RepaintClock(QObject* parent = nullptr) {
        connect(&m_timer, &QTimer::timeout, this, &RepaintClock::tick);
        m_timer.start(33);
    }
signals:
    void tick();
private:
    QTimer m_timer;
};

} // namespace nv::ui
```

- [ ] **Step 2:** `VideoTileWidget`에서 `m_timer`(33ms QTimer) 제거. 대신 생성자에 `RepaintClock&`(또는 `connect`로 외부 tick 신호) 받아 `pollFrame()`을 그 신호에 연결. 나머지(seq 비교·페인트·framePainted)는 유지.

- [ ] **Step 3:** `main.cpp`에서 `RepaintClock` 1개 생성, GridView가 타일 생성 시 그 tick에 연결되도록 배선(GridView::Callbacks 또는 생성자 인자로 RepaintClock& 전달).

- [ ] **Step 4:** 빌드+테스트 PASS, 앱 스모크(영상 표시 유지 확인) → Commit: `perf(ui): 타일별 30Hz 타이머 → 단일 RepaintClock (20ch 600Hz 발화 제거)`

---

### Task 3: SoakLogger 추출 + main.cpp 정리

**Files:** Create `src/infra/system/SoakLogger.h/.cpp`; Modify `src/ui/main.cpp`

- [ ] **Step 1:** main.cpp의 통계 타이머 람다를 `SoakLogger` 클래스로 추출 — Registryから fps 합산, RSS, 활성 채널 수를 CSV로. **회전 추가**(M2a 부채 #9): 파일 크기 상한(예: 10MB) 초과 시 `.1` 백업 후 새 파일. 경로는 절대경로(`QStandardPaths` 기반 logs 디렉토리).

- [ ] **Step 2:** main.cpp는 SoakLogger 인스턴스 생성·start만. 조립 루트 가독성 회복.

- [ ] **Step 3:** Commit: `refactor(infra): SoakLogger 추출 + csv 회전·절대경로 (main.cpp 정리)`

---

## Phase 2 — HW 디코딩

### Task 4: HwContext + FfmpegStreamSource HW 경로 + LatestSurfaceSlot

**Files:** Create `src/infra/ffmpeg/HwContext.h/.cpp`, `src/infra/video/LatestSurfaceSlot.h`; Modify `src/infra/ffmpeg/FfmpegStreamSource.h/.cpp`, `src/infra/ffmpeg/ChannelSourceFactory.h/.cpp`

**참조:** 정본 구현 https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/hw_decode.c 를 읽고 따른다 (av_hwdevice_ctx_create, get_format 콜백, av_hwframe_transfer_data). 임의 추측 금지.

- [ ] **Step 1: `HwContext`** — 플랫폼별 hwdevice 생성:
  - macOS: `AV_HWDEVICE_TYPE_VIDEOTOOLBOX`, hw 픽셀포맷 `AV_PIX_FMT_VIDEOTOOLBOX`.
  - Windows: `AV_HWDEVICE_TYPE_D3D11VA`, `AV_PIX_FMT_D3D11`.
  - `get_format` 콜백이 hw 포맷을 선택, 실패 시 SW 포맷 반환(폴백).
  - 인터페이스: `bool init(AVCodecContext*, codecId)`, `AVPixelFormat hwPixFmt()`, 정적 `getFormat` 콜백.

- [ ] **Step 2: `LatestSurfaceSlot`** — LatestFrameSlot 후속. publish가 두 종류:
  - `publishCpu(w,h,rgba)` — SW 폴백/테스트.
  - `publishGpu(w,h,handle)` — VideoToolbox CVPixelBufferRef(또는 D3D11 텍스처)의 **소유 ref 보관**(CVPixelBufferRetain/Release를 인프라 계층에서). 새 프레임 publish 시 이전 핸들 release.
  - `latest(FrameSurface& out, lastSeq)` — IFrameSurfaceRegistry가 위임.
  - 스레드 안전(mutex). GPU 핸들 수명: 소비자가 그릴 동안 유효해야 하므로 "더블버퍼"(현재 표시중 + 다음) 최소 보장 — 단순화: latest()가 ref를 하나 더 retain해서 넘기고 소비자가 release하는 계약, 또는 슬롯이 2개까지 보관. **구현 시 정확한 수명 계약을 헤더 주석에 명시.**

- [ ] **Step 3: `FfmpegStreamSource` HW 경로:**
  - open에서 HwContext.init 시도 → 성공 시 hw 디코드, 실패 시 기존 SW 경로(sws_scale→publishCpu) 유지(폴백).
  - 디코드 루프: hw 프레임이면 `av_hwframe_transfer_data` 없이 **GPU 핸들을 직접 publishGpu**(zero-copy 목표 — VideoToolbox는 frame->data[3]가 CVPixelBufferRef). transfer가 필요한 경로는 주석으로 구분.
  - `onFrameDecoded` 이벤트는 동일하게 발생(상태머신 무관).

- [ ] **Step 4: ChannelSourceFactory가 IFrameSurfaceRegistry 구현** — slot() 대신 latestSurface() 제공. LatestFrameSlot → LatestSurfaceSlot 교체.

- [ ] **Step 5:** 통합테스트(`FfmpegSourceIT`)에 HW 디코드 케이스 추가 — MediaMTX+publish로 H.264 재생, `decoded` 도달 + 슬롯에 GpuTexture 종류 서피스 도달 확인(macOS). SW 폴백 케이스도 유지.

- [ ] **Step 6:** 빌드+단위+통합 PASS → Commit: `feat(infra): VideoToolbox/D3D11VA HW 디코딩 + SW 폴백, GPU 서피스 슬롯`

---

### Task 5: RhiVideoRenderer (GPU 렌더) + SwVideoRenderer 폴백

**Files:** Create `src/app/ports/IVideoRenderer.h`, `src/ui/render/RhiVideoRenderer.h/.cpp`, `src/ui/render/SwVideoRenderer.h/.cpp`; Modify `src/ui/grid/VideoTileWidget.*`

**참조:** QRhiWidget 공식 문서(Qt 6.7+) — `initialize`/`render`(QRhiCommandBuffer), 텍스처 업로드. VideoToolbox CVPixelBuffer→Metal 텍스처 상호운용 경로 확인.

- [ ] **Step 1: `IVideoRenderer`** 포트 — `void present(const FrameSurface&)`, 위젯 위에 그림.
- [ ] **Step 2: `RhiVideoRenderer`(QRhiWidget 상속)** — GpuTexture 서피스를 GPU 텍스처로 받아 셰이더로 화면에. CVPixelBuffer→Metal은 CVMetalTextureCache 경로. 실패 시 상위가 SW 렌더러로 폴백하도록 신호.
- [ ] **Step 3: `SwVideoRenderer`** — 현 VideoTileWidget의 QPainter+QImage 로직 이관(CpuRgba 서피스용).
- [ ] **Step 4: `VideoTileWidget`** — 직접 페인트 대신 IVideoRenderer 위젯을 내부에 둠. 서피스 종류에 따라 GPU/SW 렌더러 선택(런타임 폴백).
- [ ] **Step 5:** 빌드+테스트, 앱 스모크(GPU 렌더 영상 표시) → Commit: `feat(ui): QRhi GPU 렌더러 + SW 폴백 — 프레임 카피 4→1회`

---

## Phase 3 — Windows + 성능 게이트

### Task 6: Windows 빌드 브링업 (vcpkg + D3D11VA + CI)

**Files:** Create `vcpkg.json`, `.github/workflows/ci.yml`; Modify CMake(Windows 분기)

- [ ] **Step 1: `vcpkg.json`** — FFmpeg 동적·GPL제외(x264/x265 피처 제외, avformat/avcodec/avutil/swscale + d3d11va). Qt는 별도(공식 배포).
- [ ] **Step 2:** CMake Windows 분기 — D3D11VA 경로 활성, windeployqt. macOS pkg-config 분기와 공존.
- [ ] **Step 3: CI 매트릭스** — `.github/workflows/ci.yml`: macOS + Windows, 빌드 + 단위테스트(`ctest -LE integration`). 통합테스트는 mediamtx 다운로드 가능하면 포함(아니면 단위만). **GPL 부재 검증**: 빌드 구성 문자열에 `--enable-gpl` 없음 자동 확인(설계 라이선스 요구).
- [ ] **Step 4:** macOS CI green 확인(Windows는 환경에 따라 best-effort — 보고서에 상태 기재). Commit: `build: Windows(vcpkg/D3D11VA) + GitHub Actions macOS·Windows 매트릭스 CI`

---

### Task 7: 성능 게이트 측정 + 32채널 best-effort + 수용 문서

**Files:** Modify `ops/measure-baseline.sh`; Create `docs/m2b-acceptance.md`

- [ ] **Step 1:** measure-baseline.sh에 모드 인자(sw/hw) — HW 측정 CSV를 sw와 같은 포맷으로 `logs/baseline-hw-*.csv`.
- [ ] **Step 2: `docs/m2b-acceptance.md`** 성능 게이트:
  - 20채널 동시, HW 디코딩 활성(플랫폼별 수동 확인 — Activity Monitor/GPU히스토리 또는 로그의 hwaccel 성공), 표시 확정 fps 채널당 ≥27, 드롭 < 5%.
  - **CPU/RSS가 M2a SW 베이스라인(101.8%/310MB) 대비 의미있게 감소**해야 PASS (HW 오프로딩 효과 입증). 목표: CPU 절반 이하.
  - 31~32채널 best-effort: 동작(크래시·누수 없음) 보장, 성능은 측정만 기록.
  - macOS·Windows 각각 측정란.
- [ ] **Step 3:** 실측: sim-20ch.sh로 20채널 → HW 모드 측정 → 결과 문서 기록. 32채널도 1회.
- [ ] **Step 4:** Commit: `feat(ops): 성능 게이트 측정(sw/hw) + 32ch best-effort + M2b 수용 문서`

---

### Task 8: 마무리 — README + 부채 정리 + 최종 검증

- [ ] **Step 1:** README: M2b 완료 체크, 부채 대장에서 해소 항목(#2 카피, #6 GridView→infra, #8 슬롯 수명) 정리.
- [ ] **Step 2:** 클린 빌드 + 전체 테스트(단위+통합) + 도메인/앱 순수성 재확인.
- [ ] **Step 3:** Commit: `docs: README/tech-debt — M2b 완료 상태`

---

## 완료 기준 (M2b Definition of Done)

1. 단위+통합 테스트 전체 PASS (HW 디코드·서피스·envelope·단일타이머 신규 포함)
2. **20채널 성능 게이트 통과** — HW 디코딩 활성, CPU가 M2a SW 베이스라인 대비 유의미 감소, 표시 fps 유지, macOS+Windows
3. 단일 RepaintClock으로 타이머 발화 600Hz→~30Hz
4. 프레임 경로 카피 4→1회(GPU 텍스처 직행), GridView가 IFrameSurfaceRegistry 포트에만 의존
5. channels.json 버전 envelope + 하위호환
6. CI 매트릭스 green(최소 macOS), GPL 부재 자동 검증
7. 32채널 best-effort 측정 기록

## 범위 밖 (M3)
스냅샷/녹화(정보바 버튼 활성화), 드래그앤드롭 배치, 패널/컬럼 QSettings 영속화, UI 모델 단일화(부채 #5), viewport-aware autoColumns, 전채널 다운 시각 경보.
