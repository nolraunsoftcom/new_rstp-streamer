# M2c: Zero-copy GPU 경로 (VideoToolbox → Metal 직행) + 렌더 부채 정리

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** M2b의 "동반-rgba" 우회(HW 디코드 → GPU→CPU 복사 → sws RGBA → 텍스처 재업로드)를 제거하고, VideoToolbox CVPixelBuffer(NV12)를 GPU에 머문 채 Metal 텍스처로 직접 그린다. 20채널 CPU를 추가로 끌어내리고, 이 묶음에 수렴하는 부채(#6 GridView→포트, #9/#21 present 중복·카피, #10/#22 transfer-실패, #15 zero-copy, #20 렌더러 프로브)를 함께 정리한다.

**Architecture:** 디코드 스레드는 CVPixelBuffer(NV12)를 retain해 슬롯에 게시(이미 B4가 구현). UI 스레드(렌더러)가 `IFrameSurfaceRegistry` 포트로 서피스를 받아, QRhi가 쓰는 동일 MTLDevice 기반 `CVMetalTextureCache`로 두 평면(Y=R8, CbCr=RG8)을 MTLTexture로 만들고 `QRhiTexture::createFrom`으로 래핑해 NV12→RGB 셰이더로 그린다. SW 폴백(RGBA)은 유지. 핵심: CPU 복사 0회(디코드 GPU 메모리 → 셰이더 샘플).

**Tech Stack:** M2b + CoreVideo/Metal(CVMetalTextureCache, CVMetalTextureGetTexture), QRhi `nativeHandles()`(QRhiMetalNativeHandles.dev), `QRhiTexture::createFrom({nativeHandle})`. 참조: Qt "Scene Graph - Metal Texture Import" 예제, QRhiTexture::createFrom 문서.

**측정 기준:** M2b 20채널 HW(동반-rgba) = CPU 131%. zero-copy 목표는 이보다 유의미 감소(GPU→CPU 왕복·sws·재업로드 제거분).

**브랜치:** `m2c-zerocopy` (main에서 분기)

**플랫폼 범위:** macOS(VideoToolbox/Metal) 우선. Windows(D3D11VA→D3D11 텍스처 직행)는 본 플랜 범위 밖(별도, B6에서 extractGpuHandle 플랫폼 분기는 이미 준비됨) — SW 폴백으로 동작 보장.

---

## 위험·전제 (구현자 필독)

1. **NV12 2-plane**: VideoToolbox H.264/H.265 디코드 출력은 보통 `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange`(NV12). 단일 RGBA 텍스처가 아니라 **Y 평면(R8)·CbCr 평면(RG8) 2개**를 import하고 셰이더에서 YUV→RGB 변환(BT.601/709 video-range). full vs video range 주의.
2. **MTLDevice 동일성**: CVMetalTextureCache는 QRhi가 쓰는 바로 그 `MTLDevice`로 만들어야 텍스처 공유가 된다. `rhi()->nativeHandles()` → `QRhiMetalNativeHandles::dev`. 렌더러 initialize() 시점에 캐시 생성.
3. **스레드**: CVMetalTextureCache 작업은 UI(렌더) 스레드에서. 디코드 스레드는 CVPixelBuffer retain까지만(이미 슬롯이 함). 슬롯의 releaseConsumed 계약(B4)을 이제 **실제로 사용**한다 — 렌더러가 받은 핸들을 그린 뒤 release.
4. **수명**: CVMetalTexture(래퍼)는 그것이 가리키는 MTLTexture가 GPU에서 다 쓰일 때까지 살아야 함 — 프레임 in-flight 동안 CVMetalTextureRef를 잡아둔다(QRhi 커맨드 제출~완료). 단순화: 직전 프레임 것까지 1개 더 보관(더블버퍼).
5. **폴백 불변**: zero-copy 실패(포맷 비NV12, 캐시 생성 실패, createFrom 실패) 시 기존 동반-rgba/SW 경로로 떨어지고 화면은 계속 나와야 한다.

---

### Task 1: IFrameSurfaceRegistry로 UI 전환 (부채 #6 — 선행)

zero-copy 전에 UI가 포트로 서피스를 받게 한다. 지금은 `latest(Frame&)` RGBA 호환만 쓴다.

**Files:** Modify `src/ui/grid/VideoTileWidget.h/.cpp`, `src/ui/grid/GridView.h/.cpp`, `src/ui/main.cpp`; Test `tests/unit/`

- [ ] **Step 1:** VideoTileWidget이 `LatestSurfaceSlot&` 구체타입 대신 `IFrameSurfaceRegistry&` + 자기 channelId를 받게 변경. pollFrame()이 `registry.latestSurface(channelId, surface, m_seq)`로 `FrameSurface`를 받음(CpuRgba/GpuTexture 둘 다). present(surface)로 전달. **GpuTexture면 그린 뒤 `registry.releaseConsumed(...)` 호출** (포트에 releaseConsumed 추가 — IFrameSurfaceRegistry에 `void releaseConsumed(const std::string& channelId, void* handle)` 추가, ChannelSourceFactory가 슬롯에 위임).
- [ ] **Step 2:** GridView가 ChannelSourceFactory 구체 include 제거 → `IFrameSurfaceRegistry*`만 보유. main.cpp에서 factory를 포트로 주입.
- [ ] **Step 3:** present()는 아직 RGBA만 그림(렌더러 zero-copy는 Task 3) — 이 태스크는 **배선 전환만**, 화면 동작 불변. 단위테스트: Fake registry로 VideoTileWidget이 surface를 받고 releaseConsumed를 호출하는지(GpuTexture일 때).
- [ ] **Step 4:** 빌드+테스트+실장비 스모크(영상 유지) → Commit: `refactor(ui): UI를 IFrameSurfaceRegistry 포트로 전환 + releaseConsumed 배선 (부채 #6 해소)`

---

### Task 2: VtMetalBridge — CVPixelBuffer(NV12) → MTLTexture 2장 (인프라)

**Files:** Create `src/infra/video/VtMetalBridge.h/.mm`(Obj-C++); Modify `src/CMakeLists.txt`

- [ ] **Step 1:** `VtMetalBridge`(Obj-C++ `.mm`, `#if defined(__APPLE__)`):
  - 생성: `init(void* mtlDevice)` — QRhi의 MTLDevice로 `CVMetalTextureCacheCreate`.
  - `bool map(void* cvPixelBuffer, PlaneTextures& out)` — NV12 가정, `CVMetalTextureCacheCreateTextureFromImage`로 plane 0(Y, `MTLPixelFormatR8Unorm`, full size), plane 1(CbCr, `MTLPixelFormatRG8Unorm`, half size) 생성. out에 `void* lumaTex`(MTLTexture), `void* chromaTex`, 그리고 수명 유지용 `void* lumaRef`/`void* chromaRef`(CVMetalTextureRef) 담음. 포맷이 NV12 아니면 false.
  - `void unmap(PlaneTextures&)` — CVMetalTextureRef release + cache flush.
  - 수명/스레드 주석 명시.
- [ ] **Step 2:** CMake — `.mm`을 nv_infra(또는 nv_ui? infra가 맞음)에 추가, `-framework Metal -framework CoreVideo`. Windows/기타에서는 빈 스텁.
- [ ] **Step 3:** 빌드 통과(컴파일만 — 동작은 Task 3에서). Commit: `feat(infra): VtMetalBridge — CVPixelBuffer(NV12)→Metal 2-plane 텍스처 (CVMetalTextureCache)`

---

### Task 3: RhiVideoRenderer NV12 zero-copy 경로 + 셰이더

**Files:** Modify `src/ui/render/RhiVideoRenderer.h/.cpp`; Create `src/ui/render/nv12.frag`(+ vert 재사용); Modify CMake(셰이더)

- [ ] **Step 1:** NV12→RGB 셰이더(`nv12.frag`): luma(R8) + chroma(RG8) 샘플 → BT.601/709 video-range 행렬로 RGB. 풀스크린 쿼드 vert 재사용. `qt6_add_shaders`로 .qsb.
- [ ] **Step 2:** RhiVideoRenderer:
  - initialize(): `rhi()->nativeHandles()`에서 MTLDevice 추출 → VtMetalBridge 생성. NV12 파이프라인(2 sampler) + 기존 RGBA 파이프라인 둘 다 준비.
  - present(surface): `surface.kind == GpuTexture && bridge.map(surface.gpuHandle, planes)` 성공 → NV12 경로(2 MTLTexture를 `QRhiTexture::createFrom`으로 래핑, NV12 파이프라인). 실패하거나 `kind == CpuRgba` → 기존 RGBA 경로. **그린 뒤 in-flight 동안 CVMetalTexture 유지, 다음 프레임에서 직전 것 unmap**(더블버퍼). 소비 끝나면 슬롯 releaseConsumed(상위 VideoTileWidget이 호출 — Task1 계약).
  - createFrom 시 format/pixelSize/flags 정확히 설정(문서 요구).
  - 실패 경로 전부 안전 폴백.
- [ ] **Step 3 (부채 #9/#21):** Rhi/Sw present() 공통 8줄(seq 가드·크기검증·update)을 베이스/헬퍼로 추출. RGBA 경로의 프레임당 2번째 QImage.copy()는 zero-copy 경로에선 발생 안 함(GPU 직행) — RGBA 폴백에만 남김.
- [ ] **Step 4:** 빌드+테스트, **실장비 스모크 필수**: HW 디코드 + zero-copy 렌더 + 화면 표시(스크린샷). 로그에 "render path = NV12 zero-copy" 1회 마커 추가해 확인. SW/RGBA 폴백도 NV_FORCE_SW로 확인. SIGTERM GRACEFUL-OK.
- [ ] **Step 5:** Commit: `feat(ui): NV12 zero-copy 렌더 경로 (CVPixelBuffer→Metal createFrom) + present 중복 제거`

---

### Task 4: 슬롯 불변량 정리 + 렌더러 프로브 (부채 #10/#22, #20)

**Files:** Modify `src/infra/video/LatestSurfaceSlot.h`, `src/infra/ffmpeg/FfmpegStreamSource.cpp`, `src/ui/grid/VideoTileWidget.cpp`

- [ ] **Step 1 (#10/#22):** HW transfer/디코드 실패 시 슬롯이 직전 GPU 핸들을 유지하는 경로 제거 — 실패 프레임에선 publishGpu를 호출하지 않거나, 명시적으로 직전 핸들 해제해 "슬롯 ≤1개 최신" 불변량 유지.
- [ ] **Step 2 (#20):** `selectRendererKind(true)` 하드코딩 제거 — 기동 시 1회 QRhi 가용성 프로브(첫 타일 initialize 성공/실패) 결과를 **정적 캐시**해 나머지 타일이 재사용. RHI 불가 환경에서 20타일 각자 실패→폴백(글리치 폭풍) 방지.
- [ ] **Step 3:** 빌드+테스트+스모크 → Commit: `fix: 슬롯 핸들 불변량 정리 + 렌더러 RHI 프로브 1회 캐시 (부채 #10/#20/#22)`

---

### Task 5: 측정 + 부채 정리 + 문서

**Files:** Modify `docs/m2b-acceptance.md`(zero-copy 결과란), `docs/tech-debt.md`, `README.md`

- [ ] **Step 1: 실측** — `sim-20ch.sh 20` + 시딩, zero-copy(기본) vs 동반-rgba 비교. 동반-rgba는 임시 환경변수/플래그로 강제하거나 M2b 측정값(131%)과 대조. `measure-baseline.sh`로 CPU/RSS, 표시 fps, 끊김 0 확인. 결과를 m2b-acceptance.md에 기록.
- [ ] **Step 2:** tech-debt 정리 — #15(zero-copy) 해소, #6(GridView→포트) 해소, #9/#21(present 중복·카피) 해소, #10/#22, #20 해소 표기. 남는 것 갱신.
- [ ] **Step 3:** README/순수성(`grep -rn "include <Q\|libav\|CoreVideo\|Metal\|qrhi" src/domain src/app` → 0) 재확인.
- [ ] **Step 4:** Commit: `docs: zero-copy 성능 실측 + 부채 #6/#9/#10/#15/#20/#21/#22 정리`

---

## 완료 기준

1. 단위+통합 테스트 PASS
2. **20채널 zero-copy 실측 CPU가 M2b 동반-rgba(131%) 대비 유의미 감소**, 끊김 0, 유령 0, 표시 정상(실장비)
3. CPU 복사 경로 제거(디코드 GPU 메모리 → 셰이더 직행), RGBA 경로는 SW/폴백 전용으로만 잔존
4. UI가 IFrameSurfaceRegistry 포트로만 슬롯 접근(부채 #6 실제 해소), GridView에 concrete infra include 없음
5. 부채 #6/#9/#10/#15/#20/#21/#22 해소 또는 정확히 갱신
6. zero-copy 실패 시 폴백으로 화면 보장(안전성 불변)

## 범위 밖
Windows D3D11 zero-copy(별도), M3(스냅샷/녹화·드래그·설정영속화).
