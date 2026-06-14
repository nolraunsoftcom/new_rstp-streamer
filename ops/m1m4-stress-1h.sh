#!/bin/bash
# M1~M4 통합 1시간 스트레스. 장비 READ-ONLY.
# 채널: 장비 1 (relay 경유, M4) + 합성 9 (직결, 별도 mediamtx 8654, M2 멀티채널). 전채널 녹화+스냅샷(M3).
# 단계: 지속 -> viewer churn(M4 보호막) -> 장애주입(relay 중단/재개 + 합성1개 kill/restore, M2 독립성/M1 복구) -> 마무리.
# 핵심측정: viewer 껐다켜도 장비 close ~0(M4 멱등 보호막), 채널독립성, 녹화/스냅샷 무결성, 누수0/크래시0.
set -uo pipefail
ROOT=/Users/ieonsang/nolsoft/ziilab/new_viewer-m4
APP="$ROOT/build/src/new_viewer.app/Contents/MacOS/new_viewer"
FIX="$ROOT/tests/fixtures/h264.mkv"
CFG="$HOME/Library/Preferences/영상관리시스템/channels.json"
YML="$HOME/Library/Preferences/영상관리시스템/mediamtx.yml"
LABEL=com.ziilab.newviewer.relay; PLIST=~/Library/LaunchAgents/$LABEL.plist
SNAPSRC="$HOME/Movies/new_viewer"
NSYN=9
SUS="${1:-1200}"; CHN="${2:-900}"; FAU="${3:-900}"; FIN="${4:-600}"
TS=$(date +%Y%m%d-%H%M); OUT="$ROOT/logs/m1m4-$TS"; REC="$OUT/recs"; mkdir -p "$REC" "$OUT/snaps"
export NV_RECORD_DIR="$REC/"; touch "$OUT/.start"
log(){ echo "[m1m4 $(date '+%H:%M:%S')] $*" | tee -a "$OUT/summary.txt"; }
relay_up(){ pgrep -x mediamtx | wc -l | tr -d ' '; }   # 인스턴스 수(relay+합성)
dev_close(){ grep -c "closing source pipe" "$OUT/device.log" 2>/dev/null || echo 0; }
hassrc(){ curl -s --max-time 3 http://127.0.0.1:9997/v3/paths/list 2>/dev/null | python3 -c "import sys,json;i=json.load(sys.stdin).get('items',[]);print('yes' if i and i[0].get('source') else 'no')" 2>/dev/null; }
LA=0; P=0
launch(){ "$APP" $1 >> "$OUT/viewer.log" 2>&1 & P=$!; LA=$((LA+1)); }

# ── 사전 클린 ──
pkill -x new_viewer 2>/dev/null; pkill -x mediamtx 2>/dev/null; pkill -f "ffmpeg.*127.0.0.1:8654" 2>/dev/null
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null; rm -f "$PLIST" "$YML"; sleep 2
[ -f "$CFG" ] && cp "$CFG" "$OUT/ch.bak"

# ── 합성 소스: 별도 mediamtx(8654/api9998/rtp8010/rtcp8011 — relay와 포트 분리) + 9 ffmpeg push ──
cat > "$OUT/syn.yml" <<YML
logLevel: warn
logDestinations: [stdout]
api: yes
apiAddress: 127.0.0.1:9998
rtsp: yes
rtspTransports: [tcp]
rtspAddress: 127.0.0.1:8654
rtpAddress: :8010
rtcpAddress: :8011
rtmp: no
hls: no
webrtc: no
srt: no
moq: no
metrics: no
pprof: no
playback: no
paths:
  all_others:
YML
mediamtx "$OUT/syn.yml" > "$OUT/syn-mtx.log" 2>&1 &
sleep 3
for k in $(seq 1 "$NSYN"); do
  ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i "$FIX" -c copy -f rtsp -rtsp_transport tcp "rtsp://127.0.0.1:8654/cam$k" >> "$OUT/syn-pub.log" 2>&1 &
  sleep 0.3
done
sleep 3
log "합성 소스 cam1: $(timeout 6 ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name -of csv=p=0 rtsp://127.0.0.1:8654/cam1 2>&1|head -1)"

# ── channels.json: 장비1(relay) + 합성9(직결 127.0.0.1:8654/camN) ──
python3 - "$CFG" "$NSYN" <<'PY'
import json,sys
cfg,n=sys.argv[1],int(sys.argv[2])
ch=[{"id":"dev1","name":"장비","url":"rtsp://169.254.4.1:8900/live","gridIndex":0,"useRelay":True}]
for k in range(1,n+1):
    ch.append({"id":f"syn{k}","name":f"합성{k}","url":f"rtsp://127.0.0.1:8654/cam{k}","gridIndex":k,"useRelay":False})
json.dump(ch,open(cfg,"w"),ensure_ascii=False)
print(f"channels.json: 장비1(relay) + 합성{n}(직결) = {len(ch)}채널")
PY

teardown(){
  log "=== teardown ==="
  pkill -x new_viewer 2>/dev/null
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null; rm -f "$PLIST" "$YML"
  pkill -f "ffmpeg.*127.0.0.1:8654" 2>/dev/null; pkill -x mediamtx 2>/dev/null
  [ -f "$OUT/ch.bak" ] && cp "$OUT/ch.bak" "$CFG"
  kill "$DEV" "$MON" 2>/dev/null
}
trap teardown EXIT

caffeinate -dimsu -w $$ &
adb shell "journalctl -u voxl-streamer -f --output short-iso" > "$OUT/device.log" 2>&1 & DEV=$!
( while true; do PP=$(pgrep -x new_viewer|head -1); [ -n "$PP" ] && { read R C <<<"$(ps -o rss=,%cpu= -p "$PP" 2>/dev/null)"; echo "$(date +%s) rss_mb=$((R/1024)) cpu=$C mtx=$(relay_up) hasSrc=$(hassrc)"; }; sleep 60; done ) > "$OUT/perf.log" 2>&1 & MON=$!

log "start $(date '+%F %T') 채널=10(장비1 relay+합성9 직결) 지속=${SUS} churn=${CHN} 장애=${FAU} 마무리=${FIN}"
log "장비: $(timeout 8 ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name -of csv=p=0 rtsp://169.254.4.1:8900/live 2>&1|head -1)"

# ── 1) 지속: 10채널 relay/직결 + 전채널 녹화 + 스냅샷 ──
log "[지속] 10채널 기동(viewer relay ensureUp + 직결9) ${SUS}s"
launch "--connect --auto-record --snapshot-every=30"; sleep 25
log "  mediamtx 인스턴스=$(relay_up)(relay+합성=2 기대) 장비 hasSource=$(hassrc) first_frame=$(grep -c 'first frame presented' "$OUT/viewer.log")"
sleep $((SUS-25)); kill $P 2>/dev/null; sleep 3

# ── 2) viewer churn: relay 유지(M4 보호막), 10채널 재연결 ──
log "[churn] viewer 껐다키고 (relay 유지, 장비 close 증가 없어야) ${CHN}s"
CB=$(dev_close); cend=$(( $(date +%s)+CHN )); k=0
modes=("--connect --snapshot-every=20" "--connect --record-toggle=12" "--connect --auto-record --snapshot-every=15")
while [ $(date +%s) -lt $cend ]; do k=$((k+1)); launch "${modes[$((k%3))]}"; sleep $((70+RANDOM%40)); kill $P 2>/dev/null; pkill -x new_viewer 2>/dev/null; sleep $((8+RANDOM%15)); done
CA=$(dev_close)
log "  churn 완료: 재기동 ${k}회, 장비 close 증가 $((CA-CB)) (멱등 보호막=0 기대)"

# ── 3) 장애주입: relay 중단/재개(M4) + 합성3 kill/restore(M2 독립성/M1 복구) ──
log "[장애] ${FAU}s"
launch "--connect --auto-record --snapshot-every=30"; sleep 30
log "  relay 중단(bootout) — 장비채널만 RelayDown, 합성9는 무영향(독립성)"; launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null; pkill -f "mediamtx.*$YML" 2>/dev/null; sleep 60
log "  relay 재개"; launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null; launchctl kickstart -k "gui/$(id -u)/$LABEL" 2>/dev/null; sleep 30
log "  합성 cam3 kill — syn3만 끊기고 타채널 무영향(M2 독립성)"; pkill -f "127.0.0.1:8654/cam3" 2>/dev/null; sleep 40
log "  합성 cam3 restore"; ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i "$FIX" -c copy -f rtsp -rtsp_transport tcp "rtsp://127.0.0.1:8654/cam3" >> "$OUT/syn-pub.log" 2>&1 & sleep 40
log "  장애 후 장비 hasSource=$(hassrc) mediamtx=$(relay_up)"
sleep $((FAU-200)); kill $P 2>/dev/null; sleep 3

# ── 4) 마무리 ──
log "[마무리] ${FIN}s"
launch "--connect --auto-record --snapshot-every=30"; sleep $FIN; kill $P 2>/dev/null; sleep 3

pkill -x new_viewer 2>/dev/null; sleep 2
# ── 판정 ──
log "=== 판정 ==="
log "총 viewer 기동: $LA / 비정상로그: $(grep -ciE 'crash|abort|signal SIG' "$OUT/viewer.log" 2>/dev/null)"
log "first frame presented(누적): $(grep -c 'first frame presented' "$OUT/viewer.log" 2>/dev/null)"
log "RelayDown=$(grep -c 'RelayDown' "$OUT/viewer.log" 2>/dev/null) (M4 장비채널 진단) / Failed=$(grep -c '\-> Failed' "$OUT/viewer.log" 2>/dev/null)"
log "★ 장비 close/reopen 총: $(dev_close) (churn 구간 증가 $((CA-CB)); M4 보호막)"
find "$SNAPSRC" -maxdepth 1 -name '*.png' -newer "$OUT/.start" -exec mv {} "$OUT/snaps/" \; 2>/dev/null
OK=0;BAD=0; for f in "$REC"/*.mkv; do [ -f "$f" ]||continue; if [ -s "$f" ] && ffprobe -v error -select_streams v -show_entries stream=codec_name -of csv=p=0 "$f">/dev/null 2>&1; then OK=$((OK+1)); else BAD=$((BAD+1)); fi; done
log "녹화: 재생가능 $OK / 손상 $BAD"
log "고유 녹화채널: $(ls "$REC"/*.mkv 2>/dev/null | sed -E 's@.*/@@; s@_[0-9]{8}_.*@@' | sort -u | wc -l | tr -d ' ') (10 기대)"
SOK=0; for p in "$OUT/snaps"/*.png; do [ -f "$p" ]||continue; ffprobe -v error "$p">/dev/null 2>&1 && SOK=$((SOK+1)); done
log "스냅샷: 정상 $SOK"
python3 - "$OUT/perf.log" <<'PY' 2>/dev/null | tee -a "$OUT/summary.txt" || true
import sys,re
r=[int(m.group(1)) for m in (re.search(r'rss_mb=(\d+)',l) for l in open(sys.argv[1])) if m]
c=[float(m.group(1)) for m in (re.search(r'cpu=([\d.]+)',l) for l in open(sys.argv[1])) if m]
if r: n=len(r); print(f"  RSS(MB) 시작={r[0]} 끝={r[-1]} 최대={max(r)} 전반quarter={sum(r[:max(1,n//4)])//max(1,n//4)} 후반quarter={sum(r[-max(1,n//4):])//max(1,n//4)} 샘플={n}")
if c: print(f"  CPU(%) 평균={sum(c)/len(c):.0f} 최대={max(c):.0f}")
PY
log "DONE $(date '+%F %T') — $OUT/summary.txt"
