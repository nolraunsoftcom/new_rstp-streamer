#!/bin/bash
# Fix 1 검증: 디스크풀 시 D10 백오프가 무한 churn을 멈추는지 (장비 READ-ONLY)
# 녹화만 작은 디스크 이미지로 리디렉션(NV_RECORD_DIR) — 장비/스트림은 정상 사용, 변경 없음.
set -uo pipefail
cd /Users/ieonsang/nolsoft/ziilab/new_viewer
APP="$PWD/build/src/new_viewer.app/Contents/MacOS/new_viewer"
PROCMATCH="Contents/MacOS/new_viewer"
TS=$(date +%Y%m%d-%H%M)
DIR="logs/diskfull-$TS"; mkdir -p "$DIR"
IMG="$DIR/small"          # hdiutil이 .dmg 접미사를 붙인다
MNT="/tmp/nv_diskfull_$$"

log(){ echo "[diskfull] $*" | tee -a "$DIR/summary.txt"; }
log "start $(date '+%F %T') ts=$TS"

# 1) 작은 디스크 이미지 생성 + 마운트 (6MB — h264 640x480 ~50KB/s면 ~2분에 참)
hdiutil create -size 6m -fs HFS+ -volname NVDISKFULL -o "$IMG" >/dev/null 2>&1
mkdir -p "$MNT"
hdiutil attach "$IMG.dmg" -mountpoint "$MNT" -nobrowse >/dev/null 2>&1
log "mounted 6MB image at $MNT"
df -h "$MNT" 2>/dev/null | tee -a "$DIR/summary.txt"

# 2) 녹화를 작은 이미지로 리디렉션
export NV_RECORD_DIR="$MNT/"

# 장비 로그(읽기전용) + RSS/파일수 샘플러(5초) — churn이면 파일수 폭증/RSS 불안정
adb shell "journalctl -u voxl-streamer -f --output short-iso" > "$DIR/device.log" 2>&1 &
DEV=$!
( while true; do
    PID=$(pgrep -x new_viewer | head -1)
    RSS=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ')
    NF=$(ls "$MNT"/*.mkv 2>/dev/null | wc -l | tr -d ' ')
    FREE=$(df -k "$MNT" 2>/dev/null | awk 'NR==2{print $4}')
    [ -n "$RSS" ] && echo "$(date +%s) rss_mb=$((RSS/1024)) mkv=$NF free_kb=${FREE:-?}"
    sleep 5
  done ) > "$DIR/rss.log" 2>&1 &
MON=$!

# 3) 6분 실행 — 디스크가 차고 D10 백오프가 작동/수렴하기 충분
log "run 6min: --connect --auto-record (NV_RECORD_DIR=$MNT)"
timeout 360 "$APP" --connect --auto-record 2> "$DIR/viewer.log" || true
log "run end $(date '+%F %T')"

pkill -f "$PROCMATCH" 2>/dev/null || true; sleep 3
kill "$DEV" "$MON" 2>/dev/null || true

# 4) 판정
log "=== 판정 ==="
FILES=$(ls "$MNT"/*.mkv 2>/dev/null | wc -l | tr -d ' ')
log "생성된 .mkv 수: $FILES  (무한 churn이면 수백~수천)"
ls -la "$MNT"/*.mkv 2>/dev/null | tail -20 >> "$DIR/summary.txt" || true

DISARM=$(grep -c "디스크/쓰기 오류 반복 — 녹화 중단" "$DIR/viewer.log" 2>/dev/null)
CONV=$(grep -c "디스크/쓰기 오류로 녹화 중단 — Idle로 수렴" "$DIR/viewer.log" 2>/dev/null)
RECONV=$(grep -c "녹화 중이라 표시됐으나 sink는 비녹화" "$DIR/viewer.log" 2>/dev/null)
STARTFAIL=$(grep -c "녹화 시작 실패" "$DIR/viewer.log" 2>/dev/null)
log "disarm 경고('반복 — 중단'): $DISARM"
log "D10 디스크오류 수렴: $CONV"
log "reconcile-false 수렴: $RECONV"
log "FfmpegRecorder start 실패(avio): $STARTFAIL"

CRASH=$(grep -ciE "crash|abort|terminating|signal SIG|EXC_BAD" "$DIR/viewer.log" 2>/dev/null)
log "크래시 흔적: $CRASH  (기대 0)"

log "RSS/파일수 추이(마지막 8):"
tail -8 "$DIR/rss.log" 2>/dev/null | tee -a "$DIR/summary.txt"

# RSS 누수/폭주 간이 판정: 시작/끝/최대
python3 - "$DIR/rss.log" <<'PY' 2>/dev/null | tee -a "$DIR/summary.txt" || true
import sys,re
vals=[]
for ln in open(sys.argv[1]):
    m=re.search(r'rss_mb=(\d+)',ln)
    if m: vals.append(int(m.group(1)))
if vals:
    print(f"  RSS(MB) 시작={vals[0]} 끝={vals[-1]} 최대={max(vals)} 샘플={len(vals)}")
PY

# 5) 정리 — 디스크 이미지 언마운트/삭제
hdiutil detach "$MNT" >/dev/null 2>&1 || hdiutil detach "$MNT" -force >/dev/null 2>&1
rm -f "$IMG.dmg" 2>/dev/null || true
log "DONE $(date '+%F %T') — 결과: $DIR/summary.txt"
