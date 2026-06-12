#!/bin/bash
# 20채널 시뮬: MediaMTX + ffmpeg 퍼블리셔 20개 (H.264 640x480@30 — 기준 부하)
# 사용: ./ops/sim-20ch.sh [채널수=20]   / 종료: Ctrl+C (트랩으로 일괄 정리)
set -uo pipefail
cd "$(dirname "$0")/.."
N="${1:-20}"
PORT=18600
[ -x ops/mediamtx/mediamtx ] || ./ops/mediamtx/download.sh
[ -f tests/fixtures/h264.mkv ] || ./tests/fixtures/make-fixtures.sh

cat > /tmp/nv-sim20.yml <<EOF
rtspAddress: :$PORT
api: no
rtmp: no
hls: no
webrtc: no
srt: no
paths:
  all_others:
EOF
./ops/mediamtx/mediamtx /tmp/nv-sim20.yml > /tmp/nv-sim20.log 2>&1 &
MTX=$!
sleep 1

PIDS=()
for i in $(seq 1 "$N"); do
  ffmpeg -re -stream_loop -1 -i tests/fixtures/h264.mkv -c copy \
    -f rtsp -rtsp_transport tcp "rtsp://127.0.0.1:$PORT/sim$i" >/dev/null 2>&1 &
  PIDS+=($!)
done
echo "sim ready: rtsp://127.0.0.1:$PORT/sim1 ... sim$N"
trap 'kill "${PIDS[@]}" $MTX 2>/dev/null; exit 0' INT TERM
wait
