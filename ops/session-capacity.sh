#!/bin/bash
# 실장비 동시 RTSP 세션 상한 측정(READ-ONLY — 읽기 세션만 연다).
# n=1,2,4,6,8,10,12,16,20 단계로 동시 reader를 띄워 각 15초간 프레임 수신 성공 비율을 본다.
# 모든 reader가 성공한 최대 n이 안정 상한. 하나라도 실패하면 직전 단계가 상한.
set -uo pipefail
ROOT=/Users/ieonsang/nolsoft/ziilab/new_viewer
URL="${1:-rtsp://169.254.4.1:8900/live}"
DUR="${2:-15}"
OUT="$ROOT/logs/sescap-$(date +%Y%m%d-%H%M)"; mkdir -p "$OUT"
log(){ echo "[sescap] $*" | tee -a "$OUT/summary.txt"; }
log "target=$URL dur=${DUR}s start $(date '+%T')"

reader(){ # idx → null-출력으로 프레임 수신, 성공시 frames>0
  local i="$1"
  ffmpeg -hide_banner -rtsp_transport tcp -i "$URL" -t "$DUR" -f null - \
    > "$OUT/r${LVL}_$i.log" 2>&1
}

CEIL=0
for LVL in 1 2 4 6 8 10 12 16 20; do
  log "--- level $LVL 동시 세션 ---"
  pids=()
  for i in $(seq 1 "$LVL"); do reader "$i" & pids+=($!); done
  ok=0
  for i in "${!pids[@]}"; do
    if wait "${pids[$i]}"; then
      # frame= 라인 또는 'frame=' 진행 흔적 + 에러부재로 성공 판정
      idx=$((i+1))
      if grep -qiE "Connection refused|503|Service Unavailable|timed out|Server returned 4|Server returned 5|Immediate exit|No route" "$OUT/r${LVL}_$idx.log" 2>/dev/null; then
        :
      else ok=$((ok+1)); fi
    fi
  done
  log "level $LVL: 성공 $ok / $LVL"
  if [ "$ok" -eq "$LVL" ]; then CEIL=$LVL; else
    log "level ${LVL}에서 실패 발생 → 안정 상한 = $CEIL"; break
  fi
done
log "DONE 안정 동시세션 상한 K=$CEIL  ($(date '+%T'))"
echo "$CEIL" > "$OUT/ceiling.txt"
echo "CEILING=$CEIL"
