#!/bin/bash
# 1시간 M4 relay 스트레스. 장비 READ-ONLY. relay 모드(useRelay=true), 로컬네트워크 권한 필요.
# 핵심: viewer를 수십번 껐다켜도 relay가 장비 연결을 유지 → 장비 소스파이프 close/reopen ≈0(보호막).
#       직결 8h의 81회와 대조. + 누수/크래시/녹화·스냅샷 무결성 + relay 다운→복구.
# 단계: 지속(시청+녹화+스냅샷) → viewer churn(relay 유지) → 장애주입(relay 중단/재개) → 마무리.
set -uo pipefail
ROOT=/Users/ieonsang/nolsoft/ziilab/new_viewer-m4
APP="$ROOT/build/src/new_viewer.app/Contents/MacOS/new_viewer"
CFG="$HOME/Library/Preferences/영상관리시스템/channels.json"
YML="$HOME/Library/Preferences/영상관리시스템/mediamtx.yml"
LABEL=com.ziilab.newviewer.relay
PLIST=~/Library/LaunchAgents/$LABEL.plist
SNAPSRC="$HOME/Movies/new_viewer"
API=http://127.0.0.1:9997/v3/paths/list
# 단계 시간(초): 지속 churn 장애 마무리
SUS="${1:-1200}"; CHN="${2:-900}"; FAU="${3:-900}"; FIN="${4:-600}"
TS=$(date +%Y%m%d-%H%M); OUT="$ROOT/logs/m4stress-$TS"; REC="$OUT/recs"; mkdir -p "$REC" "$OUT/snaps"
export NV_RECORD_DIR="$REC/"; touch "$OUT/.start"
log(){ echo "[m4stress $(date '+%H:%M:%S')] $*" | tee -a "$OUT/summary.txt"; }
relay_up(){ pgrep -x mediamtx >/dev/null; }
dev_close(){ grep -c "closing source pipe" "$OUT/device.log" 2>/dev/null || echo 0; }
hassrc(){ curl -s --max-time 3 "$API" 2>/dev/null | python3 -c "import sys,json;d=json.load(sys.stdin);i=[x for x in d.get('items',[]) if x['name']=='ch1'];print('yes' if i and i[0].get('source') else 'no')" 2>/dev/null; }
LA=0; P=0
# stdout+stderr 모두 로그로(명령치환 파이프를 viewer가 잡아 블로킹하는 것 방지). 전역 P/LA 설정.
launch(){ "$APP" $1 >> "$OUT/viewer.log" 2>&1 & P=$!; LA=$((LA+1)); }

# 사전 클린 + relay 모드 설정
pkill -x new_viewer 2>/dev/null; pkill -x mediamtx 2>/dev/null
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null; rm -f "$PLIST" "$YML"; sleep 2
[ -f "$CFG" ] && cp "$CFG" "$OUT/channels.bak.json"
python3 -c "import json;json.dump([{'id':'ch1','name':'장비','url':'rtsp://169.254.4.1:8900/live','gridIndex':0,'useRelay':True}],open('$CFG','w'),ensure_ascii=False)"

teardown(){ log '=== teardown ==='; pkill -x new_viewer 2>/dev/null; launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null; rm -f "$PLIST" "$YML"; [ -f "$OUT/channels.bak.json" ] && cp "$OUT/channels.bak.json" "$CFG"; kill "$DEV" "$MON" 2>/dev/null; }
trap teardown EXIT

caffeinate -dimsu -w $$ &
adb shell "journalctl -u voxl-streamer -f --output short-iso" > "$OUT/device.log" 2>&1 & DEV=$!
( while true; do P=$(pgrep -x new_viewer|head -1); [ -n "$P" ] && { read R C <<<"$(ps -o rss=,%cpu= -p $P 2>/dev/null)"; echo "$(date +%s) rss_mb=$((R/1024)) cpu=$C relay=$(relay_up&&echo 1||echo 0) src=$(hassrc)"; }; sleep 60; done ) > "$OUT/perf.log" 2>&1 & MON=$!

log "start $(date '+%F %T') 지속=${SUS} churn=${CHN} 장애=${FAU} 마무리=${FIN}"
log "장비: $(timeout 8 ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name -of csv=p=0 rtsp://169.254.4.1:8900/live 2>&1|head -1)"

# ── 1) 지속: viewer가 relay ensureUp(콜드) → 시청+녹화+스냅샷 ──
log "[지속] viewer 기동(relay ensureUp) ${SUS}s"
launch "--connect --auto-record --snapshot-every=30"; sleep 20
log "  relay UP=$(relay_up&&echo y||echo n) ch1 hasSource=$(hassrc)"
C0=$(dev_close)
sleep $((SUS-20)); kill $P 2>/dev/null; sleep 3

# ── 2) viewer churn: relay는 유지, viewer만 빠르게 껐다키고 → 장비 close 증가 없어야 ──
log "[churn] viewer 껐다키고 반복 ${CHN}s (relay 유지)  시작시 dev_close=$(dev_close)"
CB=$(dev_close); cend=$(( $(date +%s)+CHN )); k=0
modes=("--connect --snapshot-every=20" "--connect --record-toggle=10" "--connect --auto-record --snapshot-every=15")
while [ $(date +%s) -lt $cend ]; do k=$((k+1)); launch "${modes[$((k%3))]}"; sleep $((60+RANDOM%40)); kill $P 2>/dev/null; pkill -x new_viewer 2>/dev/null; sleep $((6+RANDOM%15)); done
CA=$(dev_close)
log "  churn 완료: 재기동 ${k}회, relay UP=$(relay_up&&echo y||echo n), 장비 close 증가=$((CA-CB)) (보호막=0기대)"

# ── 3) 장애주입: relay 중단→재개. viewer는 RelayDown→복구 ──
log "[장애] viewer 기동 후 relay 중단/재개 ${FAU}s"
launch "--connect --auto-record --snapshot-every=30"; sleep 30
log "  relay 중단(bootout)"; launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null; pkill -x mediamtx 2>/dev/null; sleep 60
log "  중단 중 relay UP=$(relay_up&&echo y||echo n) (n기대)"
log "  relay 재개(bootstrap+kickstart)"; launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null; launchctl kickstart -k "gui/$(id -u)/$LABEL" 2>/dev/null; sleep 60
log "  재개 후 relay UP=$(relay_up&&echo y||echo n) hasSource=$(hassrc)"
sleep $((FAU-150)); kill $P 2>/dev/null; sleep 3

# ── 4) 마무리: 지속 ──
log "[마무리] ${FIN}s"
launch "--connect --auto-record --snapshot-every=30"; sleep $FIN; kill $P 2>/dev/null; sleep 3

pkill -x new_viewer 2>/dev/null; sleep 2
# ── 판정 ──
log "=== 판정 ==="
log "총 viewer 기동: $LA / 비정상로그: $(grep -ciE 'crash|abort|signal SIG' "$OUT/viewer.log" 2>/dev/null)"
log "first frame presented: $(grep -c 'first frame presented' "$OUT/viewer.log" 2>/dev/null)"
log "RelayIntake/relay 진단: RelayDown=$(grep -c 'RelayDown' "$OUT/viewer.log" 2>/dev/null) RelayNoSource=$(grep -c 'RelayNoSource' "$OUT/viewer.log" 2>/dev/null)"
log "상태: Stalled=$(grep -c '\-> Stalled' "$OUT/viewer.log") Reconnecting=$(grep -c '\-> Reconnecting' "$OUT/viewer.log") Failed=$(grep -c '\-> Failed' "$OUT/viewer.log")"
log "★ 장비 소스파이프 close 총: $(dev_close) (직결 8h=81 대조 — churn 구간 증가 $((CA-CB)))"
find "$SNAPSRC" -maxdepth 1 -name '*.png' -newer "$OUT/.start" -exec mv {} "$OUT/snaps/" \; 2>/dev/null
OK=0;BAD=0; for f in "$REC"/*.mkv; do [ -f "$f" ]||continue; if [ -s "$f" ] && ffprobe -v error -select_streams v -show_entries stream=codec_name -of csv=p=0 "$f">/dev/null 2>&1; then OK=$((OK+1)); else BAD=$((BAD+1)); fi; done
log "녹화: 재생가능 $OK / 손상·빈 $BAD"
SOK=0; for p in "$OUT/snaps"/*.png; do [ -f "$p" ]||continue; ffprobe -v error "$p">/dev/null 2>&1 && SOK=$((SOK+1)); done
log "스냅샷: 정상 $SOK"
python3 - "$OUT/perf.log" <<'PY' 2>/dev/null | tee -a "$OUT/summary.txt" || true
import sys,re
r=[c for c in (re.search(r'rss_mb=(\d+)',l) for l in open(sys.argv[1])) if c]; r=[int(x.group(1)) for x in r]
if r: n=len(r); print(f"  RSS(MB) 시작={r[0]} 끝={r[-1]} 최대={max(r)} 전반¼={sum(r[:max(1,n//4)])//max(1,n//4)} 후반¼={sum(r[-max(1,n//4):])//max(1,n//4)} 샘플={n}")
PY
log "DONE $(date '+%F %T') — $OUT/summary.txt"
