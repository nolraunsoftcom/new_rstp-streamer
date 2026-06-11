# M1 2시간 실구동 + 스트레스 테스트 절차

목적: watchdog 없는 실장비를 상대로 viewer 단독의 안정성을 깐깐하게 검증한다.
(설계 M1의 24h 소크는 이 2h 검증 통과 후 별도 수행)

## 선행 조건
1. 장비 직결, `ffplay -rtsp_transport tcp rtsp://169.254.4.1:8900/live` 재생 확인 (완료: 2026-06-12)
2. **watchdog 미설치 상태 확인** — 이 테스트의 전제다 (`adb shell systemctl status voxl-streamer-watchdog` → not found여야 정상)
3. adb 연결 확인 (`adb devices`)

## 실행 (터미널 3개)
```bash
# T1: 장비 로그 수집
adb shell journalctl -u voxl-streamer -f --output short-iso | tee logs/device-streamer.log

# T2: viewer (자동 연결 + 구조화 로그)
./build/src/new_viewer --connect 2> logs/soak.log

# T3: 60분 경과 후 장애 주입 (전반 1시간은 무간섭 연속 구동 측정)
./ops/stress-inject.sh 6 30 300        # 30초 중단 × 6회, 5분 간격 (~35분)
# 이후 수동 2회: 케이블 분리 30초 / 장비 전원 차단 2분
```

보조 기록 (10분 간격, 별도 셸 루프 권장):
```bash
while true; do echo "$(date '+%F %T') $(netstat -an | grep -c 8900)" >> logs/netstat.log; sleep 600; done
```

## 종료 후 분석 항목 (분석은 Claude에게 로그 경로를 주고 위임)
1. **프레임 전송/수신 일치**: device-streamer.log의 송출 상태 vs soak.csv 3열(fps) — 단절 구간 제외 27~31 유지
2. **재연결 성공률**: 주입 8회(스크립트 6 + 수동 2) 전부 무개입 자동 복구 = PASS. 각 회차의 복구 소요시간 기록
3. **상태머신 정합**: soak.log의 전이 시퀀스가 설계와 일치 (가짜연결/NoPackets 판정 타이밍 포함)
4. **장비측 이상 패턴**: device-streamer.log에서 `New frame rejected`, 인코더 재오픈 실패, 세션 누적 여부
5. **자원 누수**: soak.csv 2열(RSS) 평탄(+20% 이내), netstat.log 연결 수 증가 추세 없음
6. 결과 요약을 `docs/soak-results/2026-MM-DD-m1-2h.md`로 기록
