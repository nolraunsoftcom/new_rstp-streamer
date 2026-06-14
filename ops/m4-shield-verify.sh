#!/bin/bash
# 보호막 재검증: 멱등 수정 후 viewer를 N회 껐다켜도 장비 close/reopen ≈0 인지.
# (수정 전: viewer 재시작마다 relay 재시작 → 장비 close N회. 수정 후: relay 유지 → close ≈0)
set -uo pipefail
ROOT=/Users/ieonsang/nolsoft/ziilab/new_viewer-m4
APP="$ROOT/build/src/new_viewer.app/Contents/MacOS/new_viewer"
CFG="$HOME/Library/Preferences/영상관리시스템/channels.json"
LABEL=com.ziilab.newviewer.relay; PLIST=~/Library/LaunchAgents/$LABEL.plist
YML="$HOME/Library/Preferences/영상관리시스템/mediamtx.yml"
OUT="$ROOT/logs/shield-$(date +%H%M%S)"; mkdir -p "$OUT"; export NV_RECORD_DIR="$OUT/rec/"
N="${1:-6}"
log(){ echo "[shield $(date '+%H:%M:%S')] $*" | tee -a "$OUT/summary.txt"; }
# 사전 클린 + relay 모드
pkill -x new_viewer 2>/dev/null; pkill -x mediamtx 2>/dev/null; launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null; rm -f "$PLIST" "$YML"; sleep 2
cp "$CFG" "$OUT/ch.bak"
python3 -c "import json;json.dump([{'id':'ch1','name':'장비','url':'rtsp://169.254.4.1:8900/live','gridIndex':0,'useRelay':True}],open('$CFG','w'),ensure_ascii=False)"
teardown(){ pkill -x new_viewer 2>/dev/null; launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null; rm -f "$PLIST" "$YML"; cp "$OUT/ch.bak" "$CFG"; kill $DEV 2>/dev/null; }
trap teardown EXIT
adb shell "journalctl -u voxl-streamer -f --output short-iso" > "$OUT/device.log" 2>&1 & DEV=$!
sleep 2
mtx_pid(){ pgrep -x mediamtx|head -1; }
dclose(){ grep -c "closing source pipe" "$OUT/device.log" 2>/dev/null || echo 0; }
# 콜드 스타트(relay ensureUp) + 풀 안정
log "콜드 스타트(viewer→relay ensureUp)"
"$APP" --connect >>"$OUT/viewer.log" 2>&1 & sleep 30
log "relay PID=$(mtx_pid) hasSource=$(curl -s --max-time 3 http://127.0.0.1:9997/v3/paths/list 2>/dev/null|python3 -c 'import sys,json;i=json.load(sys.stdin)["items"];print(bool(i[0].get("source")) if i else "none")' 2>/dev/null)"
PID0=$(mtx_pid); C0=$(dclose)
pkill -x new_viewer 2>/dev/null; sleep 8
# churn: N회 껐다키고 — 매번 ensureUp 호출됨(멱등이면 relay PID 유지)
log "churn ${N}회 시작 (baseline dev_close=$C0, relay PID=$PID0)"
for i in $(seq 1 "$N"); do
  "$APP" --connect >>"$OUT/viewer.log" 2>&1 & sleep 25
  log "  [$i] relay PID=$(mtx_pid) (PID0=${PID0}와 같아야 멱등) hasSource=$(curl -s --max-time 3 http://127.0.0.1:9997/v3/paths/list 2>/dev/null|python3 -c 'import sys,json;i=json.load(sys.stdin)["items"];print(bool(i[0].get("source")) if i else "none")' 2>/dev/null)"
  pkill -x new_viewer 2>/dev/null; sleep 8
done
PID1=$(mtx_pid); C1=$(dclose)
log "=== 판정 ==="
log "relay PID: 시작=$PID0 끝=$PID1 ($([ "$PID0" = "$PID1" ] && echo '동일=멱등✅' || echo '변경=재시작됨❌'))"
log "★ 장비 close/reopen: churn ${N}회 동안 증가 $((C1-C0)) (수정전=${N}, 수정후 기대 ≈0~자연stall)"
log "DONE — $OUT/summary.txt"
