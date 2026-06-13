# M1~M3 수용 결과 (2026-06-13)

대상: main HEAD `04c8a82` (M3 안정성 수정 3건 + 테스트도구 포함). 계획: `docs/2026-06-13-m1-m3-test-plan.md`.
정책: 장비(voxl-streamer) READ-ONLY. 하이브리드 소스(실장비16 + 합성4) + 클라이언트측 차단/합성 kill·restore.

## 종합: 자동·로그 검증 전 항목 PASS, UI 픽셀 항목만 권한 대기

| Phase | 항목 | 결과 | 증거 |
|---|---|---|---|
| 0 | 빌드 + 단위/통합 174 | ✅ PASS | ctest 100% (174/174) |
| 1.1 | 단일채널 재생 | ✅ PASS | 32분 무중단 + 장비 프레임 직접추출(선풍기 실내, 무손상) |
| 1.2 | HW디코드 + zero-copy | ✅ PASS | `decode path=HW(videotoolbox)` / `render path=NV12 zero-copy` |
| 1.5 | 재연결 사이클 | ✅ PASS | 합성 kill/restore 5/5 복구(first frame presented) |
| 1.6 | D1 Failed 저빈도 복귀 | ✅ PASS | 185s 드롭 → `sourceError->Failed(GaveUp)` → **정확히 60.09s 후** `->Connecting` → presented |
| 1.7 | 1시간 단일채널 안정성 | ✅ PASS | 배터리 Phase1: stall/재연결 0회, RSS 221MB 평탄, 크래시 0 |
| 2.1 | 장비 동시세션 상한 | ✅ K=16 | 1~16 전부 100%, 20세션 19/20 |
| 2.7/4.1 | 20ch 하이브리드 스케일·성능 | ✅ PASS | RSS 398→366MB(최대407) 누수0, CPU 평균111%/최대116%, 20/20 채널 녹화 무손상, 크래시0 |
| 3.1 | 녹화 충실도(1h) | ✅ PASS | 세그먼트 7개 전부 PLAYABLE, 손상0 |
| 3.2 | 스냅샷 | ✅ PASS | PNG 179장, 손상0 |
| 3.4 | 녹화 재연결 생존 | ✅ PASS | 드롭/복구마다 세그먼트 분리, 7세그먼트 손상0, 좀비0 |
| 3.5 | 녹화 토글 스트레스(15m) | ✅ PASS | 세그먼트 56개 전부 PLAYABLE, 손상0 |
| 3.6 | 디스크풀 D10 백오프 | ✅ PASS | ENOSPC 시 .mkv 유계5·disarm1·churn정지·크래시0 (수정 `bc95c9c`+`6247769` 실측 검증) |

### 무회귀 확인
M3 안정성 수정(`bc95c9c` D10백오프 / `9d787a4` 패킷기반 카운터리셋 / `6247769` healthy-reset 30s) 적용 후
배터리 카운트가 수정 전과 동일(7세그먼트/179PNG/56토글) → 녹화·재생 경로 회귀 0.

## ⏸️ 권한 대기 (UI 픽셀 항목)
터미널에 화면 기록(Screen Recording) 권한이 없어 앱 창 스크린샷 불가(메뉴바 부재로 확정). 권한 부여 + 앱 재시작 후 진행:
- 1.3 저지연 체감(주관적; 프로젝트 초기 사용자 "거의 실시간" 확인 기보유)
- 1.4 패킷/연결 표시기 위젯 렌더
- 2.3 그리드 분할 렌더(버튼 클릭 필요 — 사용자 클릭 + 내 판독)
- 2.4 UI parity(영상관리시스템·아이콘·3패널) — M2a에서 검증·수정 완료된 항목
- 3.7 MKV 크래시내성(D7): 통합테스트 REC-IT 169/170로 커버(픽스처 시 실행). 토글 스트레스의 급정지 세그먼트 56개 전부 재생가능으로 간접 뒷받침.

## 관찰된 저심각도 사항
- `libpng error: Read Error` 3회/1h — 썸네일이 쓰는 중 PNG를 읽는 양성 레이스 추정(스냅샷 179장 정상, 크래시 없음). 이번 수정과 무관(기존 동작). 후속 검토 후보.
- `device-battery.sh` rss.log 샘플러가 일부 런에서 프로세스 매칭 실패(0 기록) → `pgrep -x new_viewer`로 수정. 실 RSS는 런 중 직접 측정(221MB)으로 확인.

## 테스트 도구 (ops/)
device-battery.sh, diskfull-test.sh, synthetic-source.sh(mediamtx v1.19.1), session-capacity.sh, reconnect-test.sh(pfctl), reconnect-synthetic.sh, scale-test.sh.
