#!/bin/bash
# 하이브리드 20ch 스케일/성능 테스트: 실장비 K채널 + 합성 (TOTAL-K)채널.
# CPU%/RSS 샘플 + 채널독립성 + 녹화 동시성 판정. channels.json 백업/복원, 녹화는 임시 디렉토리.
#
# 사용법: scale-test.sh [TOTAL] [K] [DURATION_SEC]
#   K 미지정 시 최근 sescap ceiling.txt 사용(없으면 2).
set -uo pipefail
ROOT=/Users/ieonsang/nolsoft/ziilab/new_viewer
APP="$ROOT/build/src/new_viewer.app/Contents/MacOS/new_viewer"
DEVURL="rtsp://169.254.4.1:8900/live"
CFG="$HOME/Library/Preferences/영상관리시스템/channels.json"
TOTAL="${1:-20}"
K="${2:-$(cat "$(ls -dt "$ROOT"/logs/sescap-* 2>/dev/null | head -1)/ceiling.txt" 2>/dev/null || echo 2)}"
DUR="${3:-600}"
OUT="$ROOT/logs/scale-$(date +%Y%m%d-%H%M)"; mkdir -p "$OUT"
REC="$OUT/recs"; mkdir -p "$REC"
export NV_RECORD_DIR="$REC/"
log(){ echo "[scale] $*" | tee -a "$OUT/summary.txt"; }
SYN=$((TOTAL-K)); [ "$SYN" -lt 0 ] && SYN=0
log "TOTAL=$TOTAL device=K=$K synthetic=$SYN dur=${DUR}s"

# 1) 합성 소스 기동
if [ "$SYN" -gt 0 ]; then bash "$ROOT/ops/synthetic-source.sh" start "$SYN" >> "$OUT/summary.txt" 2>&1; fi

# 2) channels.json 백업 + 하이브리드 구성
[ -f "$CFG" ] && cp "$CFG" "$OUT/channels.bak.json"
python3 - "$CFG" "$K" "$SYN" "$DEVURL" <<'PY'
import json,sys
cfg,K,SYN,dev=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),sys.argv[4]
ch=[]; gi=0
for i in range(1,K+1):
    ch.append({"id":f"dev{i}","name":f"장비{i}","url":dev,"gridIndex":gi}); gi+=1
for i in range(1,SYN+1):
    ch.append({"id":f"syn{i}","name":f"합성{i}","url":f"rtsp://127.0.0.1:8554/cam{i}","gridIndex":gi}); gi+=1
import os; os.makedirs(os.path.dirname(cfg),exist_ok=True)
json.dump(ch,open(cfg,"w"),ensure_ascii=False)
print(f"channels.json: {len(ch)}채널 작성")
PY

restore(){
  pkill -x new_viewer 2>/dev/null
  [ -f "$OUT/channels.bak.json" ] && cp "$OUT/channels.bak.json" "$CFG"
  bash "$ROOT/ops/synthetic-source.sh" stop >> "$OUT/summary.txt" 2>&1
}
trap restore EXIT

# 3) RSS/CPU 샘플러
( while true; do
    PID=$(pgrep -x new_viewer | head -1)
    [ -n "$PID" ] && echo "$(date +%s) $(ps -o rss=,%cpu= -p "$PID" 2>/dev/null | awk '{print "rss_mb="int($1/1024)" cpu="$2}') est=$(netstat -an 2>/dev/null | grep -cE '8900|8554.*ESTAB')"
    sleep 10
  done ) > "$OUT/perf.log" 2>&1 &
MON=$!

# 4) 앱 기동 + 전채널 자동녹화
log "앱 기동: --connect --auto-record (${TOTAL}채널) ${DUR}s"
timeout "$DUR" "$APP" --connect --auto-record 2> "$OUT/viewer.log" || true
log "런 종료"
kill "$MON" 2>/dev/null
pkill -x new_viewer 2>/dev/null; sleep 3

# 5) 판정
log "=== 판정 ==="
OKR=0; BADR=0
for f in "$REC"/*.mkv; do [ -f "$f" ] || continue
  if ffprobe -v error -select_streams v -show_entries stream=codec_name -of csv=p=0 "$f" >/dev/null 2>&1 \
     && [ -s "$f" ]; then OKR=$((OKR+1)); else BADR=$((BADR+1)); fi
done
log "녹화 세그먼트: 재생가능 $OKR / 손상 $BADR"
log "고유 녹화채널: $(ls "$REC"/*.mkv 2>/dev/null | sed -E 's@.*/@@; s@_[0-9]{8}_.*@@' | sort -u | wc -l | tr -d ' ')"
log "crash: $(grep -ciE 'crash|abort|terminating|signal SIG' "$OUT/viewer.log" 2>/dev/null)"
log "CPU/RSS 추이(마지막 6):"; tail -6 "$OUT/perf.log" 2>/dev/null | tee -a "$OUT/summary.txt"
python3 - "$OUT/perf.log" <<'PY' 2>/dev/null | tee -a "$OUT/summary.txt" || true
import sys,re
r=[];c=[]
for ln in open(sys.argv[1]):
    m=re.search(r'rss_mb=(\d+)',ln); n=re.search(r'cpu=([\d.]+)',ln)
    if m:r.append(int(m.group(1)))
    if n:c.append(float(n.group(1)))
if r:print(f"  RSS(MB) 시작={r[0]} 끝={r[-1]} 최대={max(r)}")
if c:print(f"  CPU(%) 평균={sum(c)/len(c):.0f} 최대={max(c):.0f}")
PY
log "DONE $(date '+%T') — $OUT/summary.txt"
