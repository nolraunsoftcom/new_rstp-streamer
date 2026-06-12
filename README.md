# new_viewer

RTSP 기반 다채널 영상 관제 (재작성). 설계: `docs/2026-06-12-new-viewer-design.md`

## 빌드 & 테스트

```bash
cmake -B build -S . -DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/qt@6"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

요구사항: CMake ≥ 3.24, C++20 컴파일러, FFmpeg, Qt 6.7+. Catch2는 빌드 시 자동 다운로드(FetchContent).

통합테스트 실행: `NV_MEDIAMTX_BIN=$PWD/ops/mediamtx/mediamtx ctest --test-dir build -L integration -j1 --output-on-failure` (고정 포트 하니스 — 반드시 `-j1` 직렬 실행)

HW/SW 강제 스위치: `NV_FORCE_SW=1` 환경변수 설정 시 SW 디코딩+렌더 경로 강제 사용.

## 현재 상태

- [x] M1-코어: 도메인 상태머신 + 진단 모델 + ChannelController (FFmpeg/Qt 의존 없음)
- [x] M1-파이프라인: FFmpeg 어댑터(RTSP/TCP) + 1채널 UI + 통합 하니스
- [ ] M1 수용 1차: 실장비 2h 스트레스 실구동 + 분석 (`ops/stress-m1.md`, watchdog 미설치 전제)
- [ ] M1 수용 2차: 24h 소크
- [x] M2a: 멀티채널 그리드 (레거시 UI 패리티, SW 디코딩, 소프트한도 32)
- [x] M2b: HW 디코딩(VideoToolbox/D3D11VA) + GPU 렌더(QRhi) + Windows 구성 + 성능 게이트
