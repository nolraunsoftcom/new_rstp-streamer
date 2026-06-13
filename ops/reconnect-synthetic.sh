#!/bin/bash
# 합성 소스 kill/restore로 결정적 재연결 검증(sudo 불필요, 장비 무관).
# 상태머신(stall→Reconnecting→복구) + Fix2(패킷기반 카운터 리셋) + Phase3.4(녹화 재연결 생존)을
# 한 번에 검증한다. 녹화 ON 상태로 드롭/복구를 반복해 세그먼트가 끊김마다 분리·지속되는지 본다.
set -uo pipefail
ROOT=/Users/ieonsang/nolsoft/ziilab/new_viewer
APP="$ROOT/build/src/new_viewer.app/Contents/MacOS/new_viewer"
SYN="$ROOT/ops/synthetic-source.sh"
CFG="$HOME/Library/Preferences/영상관리시스템/channels.json"
OUT="$ROOT/logs/recon-syn-$(date +%Y%m%d-%H%M)"; mkdir -p "$OUT"
REC="$OUT/recs"; mkdir -p "$REC"; export NV_RECORD_DIR="$REC/"
log(){ echo "[recon-syn] $*" | tee -a "$OUT/summary.txt"; }
CYCLES="${1:-5}"
SLOWDROP="${2:-110}"   # slow 드롭 길이(초). Failed(maxAttempts 30×5s≈160s) 유발하려면 180 권장.

# 1) 합성 1채널 기동
bash "$SYN" start 1 >> "$OUT/summary.txt" 2>&1

# 2) channels.json 백업 + 합성 1채널 구성
[ -f "$CFG" ] && cp "$CFG" "$OUT/channels.bak.json"
python3 - "$CFG" <<'PY'
import json,sys,os
cfg=sys.argv[1]; os.makedirs(os.path.dirname(cfg),exist_ok=True)
json.dump([{"id":"syn1","name":"합성1","url":"rtsp://127.0.0.1:8554/cam1","gridIndex":0}],
          open(cfg,"w"),ensure_ascii=False)
print("channels.json: 합성1 단일채널")
PY

restore(){
  pkill -x new_viewer 2>/dev/null
  [ -f "$OUT/channels.bak.json" ] && cp "$OUT/channels.bak.json" "$CFG"
  bash "$SYN" stop >> "$OUT/summary.txt" 2>&1
}
trap restore EXIT

# 3) 앱 기동 + 자동녹화
"$APP" --connect --auto-record 2> "$OUT/viewer.log" &
APID=$!
log "앱 기동 pid=$APID — 스트리밍/녹화 안정 대기 25s"
sleep 25

# 4) fast 사이클: 드롭(18s) / 복구(24s)
for c in $(seq 1 "$CYCLES"); do
  log "[fast $c] cam1 드롭(18s)"; bash "$SYN" drop cam1 >> "$OUT/summary.txt" 2>&1; sleep 18
  log "[fast $c] cam1 복구(24s)"; bash "$SYN" restore cam1 >> "$OUT/summary.txt" 2>&1; sleep 24
done

# 5) slow 체크: 장시간 드롭(110s — 다회 재시도 누적) → 복구
log "[slow] cam1 장시간 드롭(${SLOWDROP}s) — 재시도 누적/Failed 유발"; bash "$SYN" drop cam1 >> "$OUT/summary.txt" 2>&1; sleep "$SLOWDROP"
log "[slow] cam1 복구(90s) — Failed 저빈도(60s) 자동 복귀 확인"; bash "$SYN" restore cam1 >> "$OUT/summary.txt" 2>&1; sleep 90

pkill -x new_viewer 2>/dev/null; sleep 3

# 6) 판정
log "=== 판정 ==="
log "복구(first frame presented): $(grep -c 'first frame presented' "$OUT/viewer.log" 2>/dev/null)"
log "재시도(-> Connecting): $(grep -c '\-> Connecting' "$OUT/viewer.log" 2>/dev/null)"
log "-> Reconnecting/Stalled: $(grep -cE '\-> Reconnecting|\-> Stalled' "$OUT/viewer.log" 2>/dev/null)"
log "-> Failed(저빈도 진입): $(grep -c '\-> Failed' "$OUT/viewer.log" 2>/dev/null)"
log "GaveUp 사유: $(grep -c 'GaveUp' "$OUT/viewer.log" 2>/dev/null)"
log "좀비/디스크오류 경고: $(grep -cE '좀비|디스크' "$OUT/viewer.log" 2>/dev/null)"
log "crash: $(grep -ciE 'crash|abort|terminating|signal SIG' "$OUT/viewer.log" 2>/dev/null)"
# 녹화 세그먼트(재연결마다 분리 → 여러 개, 전부 재생가능해야 함)
OK=0; BAD=0
for f in "$REC"/*.mkv; do [ -f "$f" ] || continue
  if [ -s "$f" ] && ffprobe -v error -select_streams v -show_entries stream=codec_name -of csv=p=0 "$f" >/dev/null 2>&1; then OK=$((OK+1)); else BAD=$((BAD+1)); fi
done
log "녹화 세그먼트: 재생가능 $OK / 손상·빈 $BAD  (재연결 생존 시 다수 세그먼트, 손상 0 기대)"
log "DONE $(date '+%T') — $OUT/summary.txt"
