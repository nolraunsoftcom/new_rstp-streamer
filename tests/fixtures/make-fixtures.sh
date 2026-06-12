#!/bin/bash
# 테스트 픽스처 생성: H.264 + H.265 둘 다 1급 (설계 §6).
# 주의: libx264/libx265는 픽스처 생성(개발 머신)에만 사용 — 배포 바이너리는 GPL 제외 빌드.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
[[ -f "$DIR/h264.mkv" ]] || ffmpeg -y -f lavfi -i testsrc2=size=640x480:rate=30 -t 10 \
  -c:v libx264 -pix_fmt yuv420p "$DIR/h264.mkv"
[[ -f "$DIR/h265.mkv" ]] || ffmpeg -y -f lavfi -i testsrc2=size=640x480:rate=30 -t 10 \
  -c:v libx265 -pix_fmt yuv420p -tag:v hvc1 "$DIR/h265.mkv"
echo "fixtures ready"
