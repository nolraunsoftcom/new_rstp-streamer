#!/bin/bash
# 로컬 합성 RTSP 소스 — mediamtx + ffmpeg 루프 발행으로 cam1..camN 제공.
# 스케일/펜아웃/녹화동시성 테스트용(장비 무관, 재현가능). M4 relay 사전작업도 겸함.
#
# 사용법:
#   synthetic-source.sh start N   # mediamtx 기동 + cam1..camN 발행
#   synthetic-source.sh stop      # 전체 종료(발행자 + mediamtx)
#   synthetic-source.sh drop camK # 특정 채널 발행 중단(합성 재연결/독립성 유발)
#   synthetic-source.sh restore camK <fixture> # 특정 채널 재발행
#   synthetic-source.sh status
set -uo pipefail
ROOT=/Users/ieonsang/nolsoft/ziilab/new_viewer
RUN="$ROOT/logs/synth"; mkdir -p "$RUN"
CFG="$RUN/mediamtx-test.yml"
FIX="$ROOT/tests/fixtures/h264.mkv"
PORT=8554; API=127.0.0.1:9997

write_cfg(){
  cat > "$CFG" <<YML
logLevel: warn
logDestinations: [stdout]
api: true
apiAddress: $API
rtsp: true
rtspTransports: [tcp]
rtspAddress: 127.0.0.1:$PORT
rtmp: false
hls: false
webrtc: false
srt: false
paths:
  all_others:
YML
}

pub_one(){ # camK fixture
  local cam="$1" fix="${2:-$FIX}"
  ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i "$fix" \
    -c copy -f rtsp -rtsp_transport tcp "rtsp://127.0.0.1:$PORT/$cam" \
    > "$RUN/$cam.log" 2>&1 &
  echo $! > "$RUN/$cam.pid"
}

case "${1:-}" in
  start)
    N="${2:?사용: start N}"
    [ -f "$FIX" ] || { echo "fixture 없음: $FIX"; exit 1; }
    command -v mediamtx >/dev/null || { echo "mediamtx 미설치 — brew install mediamtx"; exit 1; }
    write_cfg
    pkill -f "mediamtx $CFG" 2>/dev/null; sleep 1
    mediamtx "$CFG" > "$RUN/mediamtx.log" 2>&1 &
    echo $! > "$RUN/mediamtx.pid"
    sleep 2
    for k in $(seq 1 "$N"); do pub_one "cam$k"; sleep 0.3; done
    sleep 2
    echo "기동 완료: mediamtx + cam1..cam$N"
    # 검증: cam1 probe
    timeout 6 ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name \
      -of csv=p=0 "rtsp://127.0.0.1:$PORT/cam1" && echo "cam1 OK" || echo "cam1 probe 실패"
    ;;
  stop)
    for p in "$RUN"/cam*.pid "$RUN"/mediamtx.pid; do
      [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null; rm -f "$p"
    done
    pkill -f "ffmpeg.*127.0.0.1:$PORT" 2>/dev/null
    pkill -f "mediamtx $CFG" 2>/dev/null
    echo "합성 소스 종료"
    ;;
  drop)
    cam="${2:?사용: drop camK}"
    [ -f "$RUN/$cam.pid" ] && kill "$(cat "$RUN/$cam.pid")" 2>/dev/null && rm -f "$RUN/$cam.pid" && echo "$cam 발행 중단" || echo "$cam 없음"
    ;;
  restore)
    cam="${2:?사용: restore camK [fixture]}"; pub_one "$cam" "${3:-$FIX}"; echo "$cam 재발행"
    ;;
  status)
    echo "mediamtx: $([ -f "$RUN/mediamtx.pid" ] && ps -p "$(cat "$RUN/mediamtx.pid")" >/dev/null 2>&1 && echo up || echo down)"
    ls "$RUN"/cam*.pid 2>/dev/null | while read -r f; do
      c=$(basename "$f" .pid); echo "$c: $(ps -p "$(cat "$f")" >/dev/null 2>&1 && echo up || echo down)"
    done
    curl -s "http://$API/v3/paths/list" 2>/dev/null | head -c 400; echo
    ;;
  *) echo "사용: $0 {start N|stop|drop camK|restore camK|status}"; exit 1;;
esac
