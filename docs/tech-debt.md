# 기술 부채 대장

외부 리뷰(2026-06-12)에서 식별, 의도적으로 보류한 항목들. 해소 시점 명시.

| # | 항목 | 내용 | 해소 시점 |
|---|---|---|---|
| 1 | ECONNREFUSED 진단 매핑 | connection refused는 호스트 생존+포트 다운인데 현재 DeviceUnreachable로 뭉뚱그림. 직결=streamer 다운 / relay=RelayDown으로 세분화 필요 | M4 (relay 통합) |
| 2 | 프레임 경로 풀카피 4회 | sws→vector→slot→out→QImage. SW 경로에선 허용 범위 | M2 (HW 디코딩+GPU 렌더로 교체) |
| 3 | 통합 테스트 고정 sleep | 서버 1.5s/퍼블리셔 0.8s 대기 — 느린 CI에서 flaky 소지. 포트 폴링으로 전환 필요 | CI 도입 시 (M5) |
| 4 | 해제 후 정지화면 잔류 | disconnect 후 LatestFrameSlot에 마지막 프레임이 남아 화면 유지. 상태 표시가 있어 오인 위험 낮음 | M2 (UI 개선) |
| 5 | UI 채널 목록 3중 캐시 | MainWindow/GridView/ChannelListPanel 수동 동기화 | M3 (UI 모델 단일화) |
| 6 | GridView→infra 직접 의존 | 슬롯 조회 포트 부재 (헥사고날 위반) | M2b (렌더 경로 개편 시) |
| 7 | nextGridIndex O(n²)·전량 재직렬화 | 20~32ch 무시 가능 | 채널 수 확장 시 |
