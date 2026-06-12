#!/bin/bash
# 테스트 픽스처 생성: H.264 + H.265 둘 다 1급 (설계 §6).
# 주의: libx264/libx265는 픽스처 생성(개발 머신)에만 사용 — 배포 바이너리는 GPL 제외 빌드.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
# 키프레임 1초 간격(-g 30 / keyint=30, scenecut 끔): RTSP 중간 합류(mid-stream join) 시
# 다음 키프레임까지의 대기를 ≤1초로 제한한다. 기본 GOP(키프레임 1개/10초)은 구독자가
# 루프 중간에 합류하면 키프레임 게이트 통과까지 최대 ~10초 대기 → waitFor("packet")/녹화
# 세그먼트가 타이밍에 따라 비는 flaky 원인이었다(부채 #3 관련 안정화).
[[ -f "$DIR/h264.mkv" ]] || ffmpeg -y -f lavfi -i testsrc2=size=640x480:rate=30 -t 10 \
  -c:v libx264 -pix_fmt yuv420p -g 30 -keyint_min 30 -sc_threshold 0 "$DIR/h264.mkv"
[[ -f "$DIR/h265.mkv" ]] || ffmpeg -y -f lavfi -i testsrc2=size=640x480:rate=30 -t 10 \
  -c:v libx265 -pix_fmt yuv420p -tag:v hvc1 \
  -x265-params "keyint=30:min-keyint=30:scenecut=0" "$DIR/h265.mkv"
echo "fixtures ready"
