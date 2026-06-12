#!/bin/bash
# M1 2시간 스트레스 실구동 오케스트레이터 (ops/stress-m1.md 자동화판)
# 타임라인: 0~60분 무간섭 연속 구동 → 60분~ 장애 주입 6회(30초 중단/5분 간격) → 총 130분 후 종료
set -uo pipefail
# Mac 절전으로 타임라인이 멈추는 사고 방지 (2026-06-12 1차 런 실패 원인)
if [[ "${NV_CAFF:-}" != "1" ]] && command -v caffeinate >/dev/null 2>&1; then
  exec env NV_CAFF=1 caffeinate -i "$0" "$@"
fi
cd "$(dirname "$0")/.."
mkdir -p logs
TS=$(date '+%Y%m%d-%H%M')
echo "[orchestrator] start $TS"

# 장비 로그 수집 (adb)
adb shell "journalctl -u voxl-streamer -f --output short-iso" > "logs/device-streamer-$TS.log" 2>&1 &
DEVLOG=$!

# netstat 샘플러 (10분 간격)
( while true; do echo "$(date '+%F %T') est=$(netstat -an | grep 8900 | grep -c ESTABLISHED) all=$(netstat -an | grep -c 8900)"; sleep 600; done ) > "logs/netstat-$TS.log" 2>&1 &
NETPID=$!

# viewer 130분 (자동 연결, 창 표시 — framePresented 경로까지 실사용 동일 검증)
rm -f logs/soak.csv
timeout 7800 ./build/src/new_viewer --connect 2> "logs/soak-$TS.log" &
VIEWERPID=$!

# 1단계: 60분 무간섭
sleep 3600
echo "[orchestrator] $(date '+%T') phase2: injection start"

# 2단계: 장애 주입 6회
./ops/stress-inject.sh 6 30 300 > "logs/inject-$TS.log" 2>&1

# viewer 종료 대기
wait $VIEWERPID
EXITCODE=$?
kill $NETPID 2>/dev/null || true
kill $DEVLOG 2>/dev/null || true
mv -f logs/soak.csv "logs/soak-$TS.csv" 2>/dev/null || true
echo "[orchestrator] done $TS viewer_exit=$EXITCODE"
