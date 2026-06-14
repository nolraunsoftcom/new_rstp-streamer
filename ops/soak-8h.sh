#!/bin/bash
# 8시간 직결(mediamtx 제외) 능동 스트레스 소크. 장비 READ-ONLY.
# 구조: [지속 라운드]와 [churn 라운드] 교대 — 장시간 안정성(누수)과 껐다키고 스트레스를 둘 다 검증.
#   지속 라운드: 1회 기동으로 장시간 연속 --connect --auto-record --snapshot-every (누수/롤오버/스냅샷)
#   churn 라운드: 빠른 앱 껐다키고 반복(모드 회전: 시청 / 녹화토글 / 자동녹화), 일부는 녹화 중 강제종료
# 모니터: RSS/CPU/연결수(60s, 재시작 가로질러), 장비 journalctl(READ-ONLY). 끝에 녹화/스냅샷 무결성 검증.
# caffeinate로 슬립 방지. 인자: [총시간초] [지속라운드초] [churn라운드초] [churn당기동초]
set -uo pipefail
ROOT=/Users/ieonsang/nolsoft/ziilab/new_viewer
APP="$ROOT/build/src/new_viewer.app/Contents/MacOS/new_viewer"
CFG="$HOME/Library/Preferences/영상관리시스템/channels.json"
DUR="${1:-28800}"        # 총 8h
SUSTAIN="${2:-4200}"     # 지속 라운드 70m
CHURNDUR="${3:-1500}"    # churn 라운드 25m
CHURNRUN="${4:-120}"     # churn 1회 기동 평균 ~2m(±)
TS=$(date +%Y%m%d-%H%M)
OUT="$ROOT/logs/soak8h-$TS"; REC="$OUT/recs"; mkdir -p "$REC" "$OUT/snaps"
export NV_RECORD_DIR="$REC/"
# 스냅샷은 RecordingPaths가 Movies 고정(NV_RECORD_DIR 무시) — 이번 런 분만 마커로 식별해 수거.
SNAPSRC="$HOME/Movies/new_viewer"
touch "$OUT/.start"
log(){ echo "[soak8h $(date '+%H:%M:%S')] $*" | tee -a "$OUT/summary.txt"; }
CRASHES=0; LAUNCHES=0
END=$(( $(date +%s) + DUR ))

log "start $(date '+%F %T') 총=${DUR}s($((DUR/3600))h) 지속=${SUSTAIN}s churn=${CHURNDUR}s"
log "channels: $(cat "$CFG")"
log "장비: $(timeout 8 ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name -of csv=p=0 rtsp://169.254.4.1:8900/live 2>&1|head -1)"

# 슬립 방지(스크립트 수명 동안)
caffeinate -dimsu -w $$ &
# 장비 로그
adb shell "journalctl -u voxl-streamer -f --output short-iso" > "$OUT/device.log" 2>&1 &
DEV=$!
# RSS/CPU 샘플러(재시작 가로질러 연속)
( while true; do
    PID=$(pgrep -x new_viewer | head -1)
    [ -n "$PID" ] && { read R C <<<"$(ps -o rss=,%cpu= -p "$PID" 2>/dev/null)"; echo "$(date +%s) rss_mb=$((R/1024)) cpu=$C est=$(netstat -an 2>/dev/null|grep 8900|grep -c ESTABLISHED)"; }
    sleep 60
  done ) > "$OUT/rss.log" 2>&1 &
MON=$!

# 한 번 기동(모드, 지속초). 종료코드로 크래시 추정(0/124=정상종료/timeout, 그외=비정상).
run_app(){ # mode_args... via $1 string, dur=$2, label=$3
  local mode="$1" dur="$2" label="$3"
  LAUNCHES=$((LAUNCHES+1))
  "$APP" $mode 2>> "$OUT/viewer.log" &
  local pid=$!
  log "  [$label] 기동 pid=$pid mode[$mode] ${dur}s"
  local left=$dur
  while [ $left -gt 0 ] && kill -0 $pid 2>/dev/null; do sleep 2; left=$((left-2)); done
  if ! kill -0 $pid 2>/dev/null; then
    wait $pid; local rc=$?
    if [ $rc -ne 0 ] && [ $rc -ne 124 ] && [ $rc -ne 143 ]; then
      CRASHES=$((CRASHES+1)); log "  ⚠️ [$label] 비정상 종료 rc=$rc (크래시 의심)"
    else log "  [$label] 자체 종료 rc=$rc"; fi
  else
    kill $pid 2>/dev/null; sleep 2; pkill -x new_viewer 2>/dev/null  # 강제 off(녹화 중이면 D7 경로)
  fi
}

ROUND=0
while [ $(date +%s) -lt $END ]; do
  ROUND=$((ROUND+1))
  if [ $((ROUND % 2)) -eq 1 ]; then
    # ── 지속 라운드: 장시간 연속(시청+녹화+스냅샷) ──
    rem=$(( END - $(date +%s) )); d=$SUSTAIN; [ $d -gt $rem ] && d=$rem; [ $d -lt 30 ] && break
    log "round $ROUND [지속] ${d}s"
    run_app "--connect --auto-record --snapshot-every=30" "$d" "지속$ROUND"
    sleep $((15 + RANDOM % 30))   # off 간격
  else
    # ── churn 라운드: 빠른 껐다키고 반복(모드 회전) ──
    log "round $ROUND [churn] ${CHURNDUR}s"
    cend=$(( $(date +%s) + CHURNDUR )); k=0
    local_modes=("--connect --snapshot-every=20" "--connect --record-toggle=10" "--connect --auto-record --snapshot-every=15")
    while [ $(date +%s) -lt $cend ] && [ $(date +%s) -lt $END ]; do
      k=$((k+1)); m="${local_modes[$((k % 3))]}"
      r=$(( CHURNRUN - 40 + RANDOM % 80 ))   # 기동 시간 변동(±)
      run_app "$m" "$r" "churn$ROUND.$k"
      sleep $((8 + RANDOM % 25))   # 짧은 off 후 재기동(on)
    done
  fi
done

log "라운드 종료 $(date '+%F %T') — 정리"
pkill -x new_viewer 2>/dev/null; sleep 3
kill "$DEV" "$MON" 2>/dev/null || true

# ── 판정 ──
log "=== 판정 ==="
log "총 기동 횟수: $LAUNCHES / 비정상종료(크래시의심): $CRASHES"
log "first frame presented(누적): $(grep -c 'first frame presented' "$OUT/viewer.log" 2>/dev/null)"
log "Stalled: $(grep -c '\-> Stalled' "$OUT/viewer.log") Reconnecting: $(grep -c '\-> Reconnecting' "$OUT/viewer.log") Failed: $(grep -c '\-> Failed' "$OUT/viewer.log")"
log "crash 흔적(로그): $(grep -ciE 'crash|abort|terminating|signal SIG|EXC_BAD' "$OUT/viewer.log" 2>/dev/null)"
log "장비 reopen/error: $(grep -ciE 'reopen|wedge|failed' "$OUT/device.log" 2>/dev/null)"
# 녹화 무결성
OK=0;BAD=0
for f in "$REC"/*.mkv; do [ -f "$f" ]||continue; if [ -s "$f" ] && ffprobe -v error -select_streams v -show_entries stream=codec_name -of csv=p=0 "$f">/dev/null 2>&1; then OK=$((OK+1)); else BAD=$((BAD+1)); fi; done
log "녹화 세그먼트: 재생가능 $OK / 손상·빈 $BAD"
# 스냅샷 무결성 (Movies 고정 → 이번 런 분만 마커 이후로 수거)
find "$SNAPSRC" -maxdepth 1 -name '*.png' -newer "$OUT/.start" -exec mv {} "$OUT/snaps/" \; 2>/dev/null
SOK=0;SBAD=0
for p in "$OUT/snaps"/*.png; do [ -f "$p" ]||continue; if ffprobe -v error "$p">/dev/null 2>&1; then SOK=$((SOK+1)); else SBAD=$((SBAD+1)); fi; done
log "스냅샷: 정상 $SOK / 손상 $SBAD (수거: $OUT/snaps)"
log "녹화 총량: $(du -sh "$REC" 2>/dev/null | awk '{print $1}')"
# RSS 누수(지속 라운드 구간이 의미 — 전체 추세)
python3 - "$OUT/rss.log" <<'PY' 2>/dev/null | tee -a "$OUT/summary.txt" || true
import sys,re
rss=[];cpu=[]
for ln in open(sys.argv[1]):
    m=re.search(r'rss_mb=(\d+)',ln);c=re.search(r'cpu=([\d.]+)',ln)
    if m:rss.append(int(m.group(1)))
    if c:cpu.append(float(c.group(1)))
if rss:
    n=len(rss);h1=sum(rss[:max(1,n//4)])/max(1,n//4);h2=sum(rss[-max(1,n//4):])/max(1,n//4)
    print(f"  RSS(MB) 시작={rss[0]} 끝={rss[-1]} 최대={max(rss)} 샘플={n} 전반¼={h1:.0f} 후반¼={h2:.0f}")
if cpu:print(f"  CPU(%) 평균={sum(cpu)/len(cpu):.0f} 최대={max(cpu):.0f}")
PY
log "DONE $(date '+%F %T') — $OUT/summary.txt"
