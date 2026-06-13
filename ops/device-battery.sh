#!/bin/bash
# M3 실기기 테스트 배터리 — 1h 스트리밍+녹화+캡처, 이어서 녹화토글 스트레스
set -uo pipefail
cd /Users/ieonsang/nolsoft/ziilab/new_viewer
APP="$PWD/build/src/new_viewer.app/Contents/MacOS/new_viewer"
PROCMATCH="Contents/MacOS/new_viewer"
TS=$(date +%Y%m%d-%H%M)
DIR="logs/battery-$TS"
mkdir -p "$DIR/p1-snaps" "$DIR/p1-recs" "$DIR/p2-recs"
REC="$HOME/Movies/new_viewer"
mkdir -p "$REC"; rm -f "$REC"/*.mkv "$REC"/*.png 2>/dev/null

log(){ echo "[battery] $*" | tee -a "$DIR/summary.txt"; }
log "start $(date '+%F %T') ts=$TS"

# 장비 로그
adb shell "journalctl -u voxl-streamer -f --output short-iso" > "$DIR/device.log" 2>&1 &
DEV=$!
# RSS/연결수 샘플러(30초) — 올바른 프로세스 매칭, MB 출력
( while true; do
    PID=$(pgrep -x new_viewer | head -1)
    RSS=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ')
    [ -n "$RSS" ] && echo "$(date +%s) rss_mb=$((RSS/1024)) est=$(netstat -an|grep 8900|grep -c ESTABLISHED)"
    sleep 30
  done ) > "$DIR/rss.log" 2>&1 &
MON=$!

check_mkv(){ # dir → OK/CORRUPT 카운트 + 목록
  local d="$1" ok=0 bad=0
  for f in "$d"/*.mkv; do
    [ -f "$f" ] || continue
    local st dur
    st=$(ffprobe -v error -select_streams v -show_entries stream=codec_name -of default=nw=1:nk=1 "$f" 2>/dev/null)
    dur=$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$f" 2>/dev/null)
    if [ -n "$st" ]; then ok=$((ok+1)); echo "    $(basename "$f"): $st ${dur}s PLAYABLE" >> "$DIR/summary.txt"
    else bad=$((bad+1)); echo "    $(basename "$f"): CORRUPT" >> "$DIR/summary.txt"; fi
  done
  echo "  재생가능 $ok / 손상 $bad" >> "$DIR/summary.txt"
}

# ── Phase 1: 60분 스트리밍 + 연속 녹화 + 20초 캡처 ──
log "Phase1 (60m): --connect --auto-record --snapshot-every=20"
timeout 3600 "$APP" --connect --auto-record --snapshot-every=20 2> "$DIR/phase1-viewer.log" || true
log "Phase1 end $(date '+%F %T')"
echo "== Phase1 녹화 세그먼트 ==" >> "$DIR/summary.txt"
check_mkv "$REC"
SNAP=$(ls "$REC"/*.png 2>/dev/null | wc -l | tr -d ' ')
BAD=0; for p in "$REC"/*.png; do [ -f "$p" ] || continue; ffprobe -v error "$p" >/dev/null 2>&1 || BAD=$((BAD+1)); done
echo "  스냅샷 PNG: ${SNAP}개 (손상 ${BAD})" >> "$DIR/summary.txt"
mv "$REC"/*.mkv "$DIR/p1-recs/" 2>/dev/null || true
mv "$REC"/*.png "$DIR/p1-snaps/" 2>/dev/null || true

pkill -f "$PROCMATCH" 2>/dev/null || true; sleep 5

# ── Phase 2: 15분 녹화 토글 스트레스(8초) ──
log "Phase2 (15m): --connect --record-toggle=8"
timeout 900 "$APP" --connect --record-toggle=8 2> "$DIR/phase2-viewer.log" || true
log "Phase2 end $(date '+%F %T')"
echo "== Phase2 토글 세그먼트 ==" >> "$DIR/summary.txt"
check_mkv "$REC"
mv "$REC"/*.mkv "$DIR/p2-recs/" 2>/dev/null || true

pkill -f "$PROCMATCH" 2>/dev/null || true
kill $DEV $MON 2>/dev/null || true
log "DONE $(date '+%F %T') — 결과: $DIR/summary.txt"
