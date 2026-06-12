#!/bin/bash
# 20채널 SW 디코딩 베이스라인 측정 (M2b 성능 게이트의 비교 기준)
# 전제: sim-20ch.sh 가동 중 + viewer에 sim1~sim20 채널 등록·연결 완료 상태에서 실행
# 사용: ./ops/measure-baseline.sh [측정초=300]
set -uo pipefail
cd "$(dirname "$0")/.."
DUR="${1:-300}"
PID=$(pgrep -f "new_viewer" | head -1)
[ -n "$PID" ] || { echo "new_viewer 미실행"; exit 1; }
OUT="logs/baseline-sw-$(date +%Y%m%d-%H%M).csv"
echo "ts,cpu_pct,rss_mb" > "$OUT"
END=$((SECONDS + DUR))
while [ $SECONDS -lt $END ]; do
  LINE=$(ps -o %cpu=,rss= -p "$PID")
  echo "$(date +%s),$(echo "$LINE" | awk '{print $1","$2/1024}')" >> "$OUT"
  sleep 5
done
awk -F, 'NR>1{c+=$2; r+=$3; n++} END{printf "평균 CPU %.1f%%, 평균 RSS %.1fMB (n=%d)\n", c/n, r/n, n}' "$OUT"
echo "saved: $OUT"
