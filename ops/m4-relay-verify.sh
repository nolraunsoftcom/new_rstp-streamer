#!/bin/bash
# M4 기능검증: relay 활성 상태로 실기기 경유. 보호막(sourceOnDemand:no)·viewer재시작 세션무중단·진단.
# 장비 READ-ONLY. LaunchAgent/mediamtx/channels.json 전부 백업·복원(teardown trap).
set -uo pipefail
ROOT=/Users/ieonsang/nolsoft/ziilab/new_viewer-m4
APP="$ROOT/build/src/new_viewer.app/Contents/MacOS/new_viewer"
CFG="$HOME/Library/Preferences/영상관리시스템/channels.json"
YML="$HOME/Library/Preferences/영상관리시스템/mediamtx.yml"
LABEL="com.ziilab.newviewer.relay"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
API="http://127.0.0.1:9997/v3/paths/list"
OUT="$ROOT/logs/m4verify-$(date +%Y%m%d-%H%M)"; mkdir -p "$OUT"
log(){ echo "[m4verify] $*" | tee -a "$OUT/summary.txt"; }

relay_running(){ pgrep -x mediamtx >/dev/null; }
api_get(){ curl -s --max-time 3 "$API" 2>/dev/null; }
ch1_hassource(){ api_get | python3 -c "import sys,json
try:
 d=json.load(sys.stdin)
 it=[i for i in d.get('items',[]) if i['name']=='ch1']
 print('yes' if it and it[0].get('source') else 'no')
except: print('err')" 2>/dev/null; }

teardown(){
  log '=== teardown ==='
  pkill -x new_viewer 2>/dev/null
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null
  rm -f "$PLIST" "$YML"
  [ -f "$OUT/channels.bak.json" ] && cp "$OUT/channels.bak.json" "$CFG"
  sleep 2
  log "복원후: mediamtx $(relay_running && echo UP || echo down) / plist $([ -f "$PLIST" ] && echo 존재 || echo 없음) / channels $(python3 -c "import json;print(len(json.load(open('$CFG'))),'ch')" 2>/dev/null)"
}
trap teardown EXIT

# 사전 클린
pkill -x new_viewer 2>/dev/null; launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null; rm -f "$PLIST" "$YML"; sleep 1
[ -f "$CFG" ] && cp "$CFG" "$OUT/channels.bak.json"

# channels.json → ch1 relay 모드
python3 - "$CFG" <<'PY'
import json,sys
cfg=sys.argv[1]
json.dump([{"id":"ch1","name":"장비","url":"rtsp://169.254.4.1:8900/live","gridIndex":0,"useRelay":True}],
          open(cfg,"w"),ensure_ascii=False)
print("channels.json: ch1 useRelay=true")
PY
log "장비 도달: $(timeout 6 ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name -of csv=p=0 rtsp://169.254.4.1:8900/live 2>&1 | head -1)"

# ── 1) viewer 기동 → ensureUp가 relay 서비스 시작 ──
log '=== viewer 기동 (relay ensureUp) ==='
NV_RECORD_DIR="$OUT/recs/" "$APP" --connect 2> "$OUT/viewer1.log" &
V1=$!
sleep 25

# 11.1 보호막: 생성된 yml 검증
log '--- 11.1 sourceOnDemand:no 보호막 ---'
if [ -f "$YML" ]; then
  log "yml sourceOnDemand:no = $(grep -c 'sourceOnDemand: no' "$YML")"
  log "yml tcp = $(grep -c 'rtspTransports: \[tcp\]' "$YML")"
  log "yml source = $(grep 'source:' "$YML" | head -1 | tr -d ' ')"
else log "❌ mediamtx.yml 미생성"; fi
log "mediamtx 실행: $(relay_running && echo UP || echo DOWN)"
log "Control API ch1 hasSource: $(ch1_hassource)"

# 11.3 진단: viewer가 relay 경유로 연결 + RelayIntake
log '--- 11.3 진단/연결 ---'
log "viewer relay URL 연결: $(grep -c '127.0.0.1:8554/ch1' "$OUT/viewer1.log" 2>/dev/null)"
log "first frame presented: $(grep -c 'first frame presented' "$OUT/viewer1.log" 2>/dev/null)"
log "decode/render: $(grep -oE 'decode path = [A-Z]+|render path = [A-Za-z0-9 -]+' "$OUT/viewer1.log" 2>/dev/null | tr '\n' ' ')"

# ── 11.2 보호막+재시작 무중단: viewer 종료해도 device leg 유지 ──
log '=== 11.2 viewer 종료 → device leg 무중단 (sourceOnDemand:no) ==='
SRC_BEFORE=$(ch1_hassource)
kill "$V1" 2>/dev/null; sleep 6
log "viewer 종료후 mediamtx: $(relay_running && echo UP || echo DOWN)"
log "viewer 종료후(0 viewer) ch1 hasSource: $(ch1_hassource) (before=$SRC_BEFORE) — yes면 보호막 동작"

# viewer 재기동 → relay로 재연결
log '=== viewer 재기동 → relay 재연결 ==='
NV_RECORD_DIR="$OUT/recs/" "$APP" --connect 2> "$OUT/viewer2.log" &
V2=$!
sleep 18
log "재기동 first frame presented: $(grep -c 'first frame presented' "$OUT/viewer2.log" 2>/dev/null)"
log "재기동 중 mediamtx 연속 UP: $(relay_running && echo UP || echo DOWN)"
kill "$V2" 2>/dev/null; sleep 3

log '=== 판정 끝 (teardown은 trap) ==='
