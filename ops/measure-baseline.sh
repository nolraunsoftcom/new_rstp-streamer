#!/bin/bash
# 베이스라인 측정 (HW / SW 모드) — M2b 성능 게이트 비교용
# 전제: sim-20ch.sh 가동 중 + viewer에 채널 등록·연결 완료 상태에서 실행
# 사용: ./ops/measure-baseline.sh <mode=hw|sw> [측정초=300]
#   hw — 기본 실행 (VideoToolbox/D3D11VA 활성)
#   sw — NV_FORCE_SW=1 로 viewer 재기동 후 측정 (SW 폴백 강제)
# 출력 CSV: logs/baseline-<mode>-<ts>.csv  (포맷 동일: ts,cpu_pct,rss_mb)
set -uo pipefail
cd "$(dirname "$0")/.."

MODE="${1:-hw}"
DUR="${2:-300}"

if [[ "$MODE" != "hw" && "$MODE" != "sw" ]]; then
  echo "사용: $0 <hw|sw> [측정초=300]"
  exit 1
fi

PID=$(pgrep -f "new_viewer" | head -1)
[ -n "$PID" ] || { echo "new_viewer 미실행"; exit 1; }

OUT="logs/baseline-${MODE}-$(date +%Y%m%d-%H%M).csv"
echo "ts,cpu_pct,rss_mb" > "$OUT"
echo "측정 시작: mode=$MODE pid=$PID dur=${DUR}s → $OUT"

END=$((SECONDS + DUR))
while [ $SECONDS -lt $END ]; do
  LINE=$(ps -o %cpu=,rss= -p "$PID" 2>/dev/null) || { echo "프로세스 종료됨"; break; }
  echo "$(date +%s),$(echo "$LINE" | awk '{print $1","$2/1024}')" >> "$OUT"
  sleep 5
done

awk -F, 'NR>1{c+=$2; r+=$3; n++} END{printf "평균 CPU %.1f%%, 평균 RSS %.1fMB (n=%d)\n", c/n, r/n, n}' "$OUT"
echo "saved: $OUT"
