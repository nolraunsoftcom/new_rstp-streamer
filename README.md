# new_viewer

RTSP 기반 다채널 영상 관제 (재작성). 설계: `docs/2026-06-12-new-viewer-design.md`

## 빌드 & 테스트

```bash
cmake -B build -S .
cmake --build build -j
ctest --test-dir build --output-on-failure
```

요구사항: CMake ≥ 3.24, C++20 컴파일러. Catch2는 빌드 시 자동 다운로드(FetchContent).

## 현재 상태

- [x] M1-코어: 도메인 상태머신 + 진단 모델 + ChannelController (FFmpeg/Qt 의존 없음)
- [ ] M1-파이프라인: FFmpeg 어댑터 + 최소 UI + 통합테스트 + 실장비 소크
