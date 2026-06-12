#!/bin/bash
# 장비측 장애 주입 사이클: voxl-streamer를 멈췄다 살리며 viewer의 무개입 자동 복구를 검증.
# watchdog 미설치 전제 (2026-06-12 결정) — 인코더 재오픈 wedge가 나도 viewer가 견뎌야 한다.
# 사용: ./stress-inject.sh [사이클수=10] [중단초=30] [사이클간격초=300]
set -euo pipefail
N="${1:-10}"; DOWN="${2:-30}"; INTERVAL="${3:-300}"
for i in $(seq 1 "$N"); do
  echo "[$(date '+%F %T')] cycle $i/$N: streamer stop ${DOWN}s"
  adb shell systemctl stop voxl-streamer
  sleep "$DOWN"
  echo "[$(date '+%F %T')] cycle $i/$N: streamer start"
  adb shell systemctl start voxl-streamer
  sleep "$INTERVAL"
done
echo "[$(date '+%F %T')] done"
