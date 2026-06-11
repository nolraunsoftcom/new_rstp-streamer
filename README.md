# new_viewer

RTSP 기반 다채널 영상 관제 (재작성). 설계: `docs/2026-06-12-new-viewer-design.md`

## 빌드 & 테스트

```bash
cmake -B build -S . -DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/qt@6"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

요구사항: CMake ≥ 3.24, C++20 컴파일러, FFmpeg, Qt 6.7+. Catch2는 빌드 시 자동 다운로드(FetchContent).

통합테스트 실행: `NV_MEDIAMTX_BIN=$PWD/ops/mediamtx/mediamtx ctest --test-dir build -L integration --output-on-failure`

## 현재 상태

- [x] M1-코어: 도메인 상태머신 + 진단 모델 + ChannelController (FFmpeg/Qt 의존 없음)
- [x] M1-파이프라인: FFmpeg 어댑터(RTSP/TCP) + 1채널 UI + 통합 하니스
- [ ] M1 수용 1차: 실장비 2h 스트레스 실구동 + 분석 (`ops/stress-m1.md`, watchdog 미설치 전제)
- [ ] M1 수용 2차: 24h 소크
- [ ] M2: 멀티채널 그리드 + HW 디코딩/GPU 렌더 + 성능 게이트
