#!/bin/bash
# 클라이언트측 pfctl 차단으로 결정적 재연결 유발(장비 READ-ONLY — 장비 무변경).
# 앱(--connect)을 띄우고 장비 IP로 가는 TCP를 N회 차단/해제하며 상태머신 거동을 로그로 판정.
#
# 사용법: reconnect-test.sh [cycles] [block_sec] [recover_sec] [mode]
#   mode=fast(기본): block_sec 짧게(15s) — Stalled→Reconnecting→복구 사이클 검증(1.5)
#   mode=slow      : block_sec 길게(70s) — maxAttempts 초과→Failed(저빈도)→해제 후 복귀(1.6)
# pfctl은 sudo 필요. 원래 룰셋/활성상태를 저장 후 종료 시 복원한다.
set -uo pipefail
ROOT=/Users/ieonsang/nolsoft/ziilab/new_viewer
APP="$ROOT/build/src/new_viewer.app/Contents/MacOS/new_viewer"
DEVIP="${DEVIP:-169.254.4.1}"
CYCLES="${1:-5}"; BLOCK="${2:-15}"; RECOVER="${3:-20}"; MODE="${4:-fast}"
OUT="$ROOT/logs/reconnect-$(date +%Y%m%d-%H%M)"; mkdir -p "$OUT"
SAVED="$OUT/pf-saved.conf"
log(){ echo "[reconnect] $*" | tee -a "$OUT/summary.txt"; }

# sudo 확인
if ! sudo -n true 2>/dev/null; then
  echo "이 스크립트는 pfctl(sudo)이 필요합니다. 세션에서 다음으로 실행하세요:"
  echo "  !sudo -v && bash ops/reconnect-test.sh $CYCLES $BLOCK $RECOVER $MODE"
  exit 2
fi

# 원래 pf 상태 저장
PF_WAS=$(sudo pfctl -s info 2>/dev/null | awk '/Status:/{print $2}')   # Enabled/Disabled
sudo pfctl -sr > "$SAVED" 2>/dev/null || true
log "pf 원상태=$PF_WAS, 룰셋 저장=$SAVED"

block(){ { cat "$SAVED"; echo "block drop quick proto tcp from any to $DEVIP"; } | sudo pfctl -f - 2>/dev/null; sudo pfctl -e 2>/dev/null || true; }
unblock(){ sudo pfctl -f "$SAVED" 2>/dev/null || true; }
restore(){ sudo pfctl -f "$SAVED" 2>/dev/null || true; [ "$PF_WAS" = "Disabled" ] && sudo pfctl -d 2>/dev/null || true; }
trap 'restore; pkill -x new_viewer 2>/dev/null; rm -f "$SAVED"' EXIT

[ "$MODE" = "slow" ] && [ "$BLOCK" -lt 70 ] && BLOCK=70
log "cycles=$CYCLES block=${BLOCK}s recover=${RECOVER}s mode=$MODE dev=$DEVIP"

# 앱 기동
"$APP" --connect 2> "$OUT/viewer.log" &
APID=$!
log "앱 기동 pid=$APID — 초기 스트리밍 대기 20s"
sleep 20

for c in $(seq 1 "$CYCLES"); do
  log "[cycle $c] 차단 ${BLOCK}s"
  block; sleep "$BLOCK"
  log "[cycle $c] 해제, 복구 ${RECOVER}s"
  unblock; sleep "$RECOVER"
done

sleep 5
pkill -x new_viewer 2>/dev/null; sleep 2

# 판정 (상태머신/컨트롤러 로그 키워드)
log "=== 판정 ==="
log "Streaming 진입: $(grep -c 'Streaming' "$OUT/viewer.log" 2>/dev/null)"
log "Reconnecting/Stalled: $(grep -cE 'Reconnecting|Stalled' "$OUT/viewer.log" 2>/dev/null)"
log "재연결 성공(복구): $(grep -c 'first frame presented\|state.*Streaming' "$OUT/viewer.log" 2>/dev/null)"
GAVEUP=$(grep -c 'GaveUp\|Failed' "$OUT/viewer.log" 2>/dev/null)
log "Failed/GaveUp: $GAVEUP  (fast모드 기대 0, slow모드 기대 ≥1 후 복귀)"
log "crash: $(grep -ciE 'crash|abort|terminating|signal SIG' "$OUT/viewer.log" 2>/dev/null)"
log "DONE $(date '+%T') — $OUT/summary.txt"
