#!/bin/bash
# M3 실기기 테스트 배터리 — 1h 스트리밍+녹화+캡처, 이어서 녹화토글 스트레스
set -uo pipefail
cd /Users/ieonsang/nolsoft/ziilab/new_viewer
APP=build/src/new_viewer.app/Contents/MacOS/new_viewer
TS=$(date +%Y%m%d-%H%M)
mkdir -p logs "logs/battery-$TS"
DIR="logs/battery-$TS"
REC=~/Movies/new_viewer
rm -f "$REC"/*.mkv "$REC"/*.png 2>/dev/null

echo "[battery] start $(date '+%F %T') ts=$TS" | tee "$DIR/summary.txt"

# 장비 로그 수집
adb shell "journalctl -u voxl-streamer -f --output short-iso" > "$DIR/device.log" 2>&1 &
DEV=$!
# RSS/연결수 샘플러(30초)
( while true; do P=$(pgrep -f "new_viewer --connect" | head -1); R=$(ps -o rss= -p "$P" 2>/dev/null); echo "$(date +%s) rss_kb=$R est=$(netstat -an|grep 8900|grep -c ESTABLISHED)"; sleep 30; done ) > "$DIR/rss.log" 2>&1 &
MON=$!

# ── Phase 1: 60분 스트리밍 + 연속 녹화(끊김 통과) + 20초마다 캡처 ──
echo "[battery] Phase1 (60m): --connect --auto-record --snapshot-every=20" | tee -a "$DIR/summary.txt"
timeout 3600 $APP --connect --auto-record --snapshot-every=20 2> "$DIR/phase1-viewer.log"
echo "[battery] Phase1 end $(date '+%F %T')" | tee -a "$DIR/summary.txt"
# Phase1 산출물 무결성 검사
echo "== Phase1 산출물 ==" >> "$DIR/summary.txt"
ls -la "$REC"/*.mkv 2>/dev/null | tee -a "$DIR/summary.txt"
for f in "$REC"/*.mkv; do
  [ -f "$f" ] || continue
  DUR=$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$f" 2>/dev/null)
  ST=$(ffprobe -v error -select_streams v -show_entries stream=codec_name -of default=nw=1:nk=1 "$f" 2>/dev/null)
  echo "  $(basename "$f"): codec=$ST dur=${DUR}s $([ -n "$ST" ] && echo PLAYABLE || echo CORRUPT)" >> "$DIR/summary.txt"
done
echo "  스냅샷 PNG: $(ls "$REC"/*.png 2>/dev/null | wc -l | tr -d ' ')개" >> "$DIR/summary.txt"
# 스냅샷 유효성(손상 PNG 검출)
BAD=0; for p in "$REC"/*.png; do [ -f "$p" ] || continue; ffprobe -v error "$p" >/dev/null 2>&1 || BAD=$((BAD+1)); done
echo "  손상 PNG: $BAD개" >> "$DIR/summary.txt"
mv "$REC"/*.mkv "$DIR/" 2>/dev/null; mv "$REC"/*.png "$DIR/p1-snaps/" 2>/dev/null || { mkdir -p "$DIR/p1-snaps"; mv "$REC"/*.png "$DIR/p1-snaps/" 2>/dev/null; }

sleep 5
# ── Phase 2: 15분 녹화 토글 스트레스(8초 주기) ──
echo "[battery] Phase2 (15m): --connect --record-toggle=8" | tee -a "$DIR/summary.txt"
timeout 900 $APP --connect --record-toggle=8 2> "$DIR/phase2-viewer.log"
echo "[battery] Phase2 end $(date '+%F %T')" | tee -a "$DIR/summary.txt"
echo "== Phase2 산출물(토글 세그먼트) ==" >> "$DIR/summary.txt"
P2OK=0; P2BAD=0
for f in "$REC"/*.mkv; do
  [ -f "$f" ] || continue
  ST=$(ffprobe -v error -select_streams v -show_entries stream=codec_name -of default=nw=1:nk=1 "$f" 2>/dev/null)
  [ -n "$ST" ] && P2OK=$((P2OK+1)) || P2BAD=$((P2BAD+1))
done
echo "  토글 MKV: 재생가능 $P2OK / 손상 $P2BAD" >> "$DIR/summary.txt"
mkdir -p "$DIR/p2-recs"; mv "$REC"/*.mkv "$DIR/p2-recs/" 2>/dev/null

kill $DEV $MON 2>/dev/null
echo "[battery] DONE $(date '+%F %T') — 결과: $DIR/summary.txt" | tee -a "$DIR/summary.txt"
