# 기술 부채 대장

외부 리뷰(2026-06-12)에서 식별, 의도적으로 보류한 항목들. 해소 시점 명시.

| # | 항목 | 내용 | 해소 시점 |
|---|---|---|---|
| 1 | ECONNREFUSED 진단 매핑 | connection refused는 호스트 생존+포트 다운인데 현재 DeviceUnreachable로 뭉뚱그림. 직결=streamer 다운 / relay=RelayDown으로 세분화 필요 | M4 (relay 통합) |
| 2 | 프레임 경로 풀카피 4회 | sws→vector→slot→out→QImage. SW 경로에선 허용 범위 | **부분 해소(M2b)**: HW 경로 GPU 서피스 직행으로 4→1회 감소. 단 동반-rgba(CPU 왕복) 미제거 — zero-copy는 #15로 이월 |
| 3 | 통합 테스트 고정 sleep | 서버 1.5s/퍼블리셔 0.8s 대기 — 느린 CI에서 flaky 소지. 포트 폴링으로 전환 필요 | CI 도입 시 (M5) |
| 4 | 해제 후 정지화면 잔류 | disconnect 후 LatestFrameSlot에 마지막 프레임이 남아 화면 유지. 상태 표시가 있어 오인 위험 낮음 | M2 (UI 개선) |
| 5 | UI 채널 목록 3중 캐시 | MainWindow/GridView/ChannelListPanel 수동 동기화 | M3 (UI 모델 단일화) |
| 6 | GridView→infra 직접 의존 | 슬롯 조회 포트 부재 (헥사고날 위반) | **부분 해소(M2b)**: 포트(IFrameSurfaceRegistry)는 추가됐으나 GridView가 아직 concrete ChannelSourceFactory.slot() 사용. UI 전환은 zero-copy 작업과 함께 |
| 7 | nextGridIndex O(n²)·전량 재직렬화 | 20~32ch 무시 가능 | 채널 수 확장 시 |
| 8 | 슬롯 레지스트리 무한 증가 | destroySource no-op + chN ID 미재사용 — 추가/삭제 반복 시 슬롯 누적 | M2b (IFrameSurfaceRegistry 포트화 시 수명 정리) |
| 9 | soak.csv 무한 append·상대경로 | 회전/상한 없음 | M2b ride-along (SoakLogger 추출) |
| 10 | URL 평문 표시 | rtsp://user:pass@ 자격증명 화면 노출 | M2b ride-along (마스킹) |
| 11 | 패널/컬럼 선택 미영속 | QSettings 저장 안 함 — 재시작 리셋 | M3 |

## M2a 최종 리뷰(2026-06-12) 잔여 — 머지 승인됨, 비차단

| # | 항목 | 내용 | 해소 |
|---|---|---|---|
| 12 | CompositeLogger::setCallback 스레드 안전성 | log()와 setCallback 간 미동기화 — 현재 teardown 순서(drain 후 호출)로 레이스 창 near-zero | **해소(B3/M2b)**: SoakLogger 추출 시 atomic 처리 적용 확인 |
| 13 | GridView m_tiles QString 키 | relayout 핫패스에서 fromStdString 반복 — 32ch에서 무영향 | **해소(B5/M2b)**: RhiVideoRenderer/SwVideoRenderer 도입 시 std::string 키로 전환 |
| 14 | 상태→한글/색 매핑 중복 | GridView·ChannelListPanel 2곳 복제 | M2b ride-along (StatusDisplay 추출) |

## M2b 실측 후 (2026-06-12)

| # | 항목 | 내용 | 해소 |
|---|---|---|---|
| 15 | HW 디코드 GPU→CPU 왕복 | VideoToolbox 디코드 후 av_hwframe_transfer_data로 CPU 복사 + sws RGBA + 텍스처 재업로드(동반-rgba). 진짜 zero-copy(CVPixelBuffer→CVMetalTextureCache) 미적용 — 20ch CPU의 주 오버헤드 | M2b 후속 성능 작업 (가장 큰 CPU 레버) |
| 16 | 32ch 성능 미측정 | 시뮬 32 퍼블리셔가 측정 머신 포화 — 별도 부하 발생원/세션 필요 | 후속 (분리 측정) |
| 17 | GPU framePainted 가시성 의존 | RhiVideoRenderer.render()가 compositing 시에만 호출 → 오프스크린/최소화 창은 표시단계 미확정(구 SW paintEvent도 동일 특성, 회귀 아님) | M3 (필요 시 보정) |
| 18 | 저장 실패 UI 미표시 | save 실패가 로그 한 줄뿐 — 상태바/다이얼로그 미반영 | M3 (UI 알림) |
| 19 | placeholder 풀 미축소 | hide만, 최대 셀 수로 유한 | 경미, 보류 |
| 20 | 렌더러 RHI 프로브 미캐시 | selectRendererKind(true) 하드코딩, 타일별 폴백·미전역캐시 | zero-copy 묶음 |
| 21 | present() 중복 + 프레임당 2번째 QImage 카피 | Rhi/Sw 8줄 복제, 초당 600 카피 | zero-copy 묶음 |
| 22 | HW transfer 실패 시 슬롯 직전 핸들 잔존 | seq 가드로 무해, 불변량 흐림 | zero-copy 묶음 |

## M2c 해소 (2026-06-13, zero-copy)

| # | 항목 | 상태 |
|---|---|---|
| 6 | GridView→infra 직접 의존 | **해소** — UI가 IFrameSurfaceRegistry 포트로 전환(C1), GridView의 ChannelSourceFactory include 0 |
| 9/21 | present() 중복 + 프레임당 2번째 QImage 카피 | **해소** — 공통부 추출, zero-copy 경로는 GPU 직행(카피 없음), RGBA 카피는 SW 폴백만 |
| 10/22 | HW transfer 실패 시 슬롯 핸들 잔존 | **해소/무해확인**(C4) — 불변량 주석 명시, seq 가드로 소비 차단 |
| 15 | HW 디코드 GPU→CPU 왕복 | **해소** — NV12 CVPixelBuffer→Metal createFrom 직행(C2/C3), 20ch CPU 131%→92.6% |
| 20 | 렌더러 RHI 프로브 미캐시 | **해소**(C4) — 1회 프로브 정적 캐시, 타일별 폴백 폭풍 방지 |

남은 부채: #12(CompositeLogger, 해소됨/B3), #13(QString키, 해소됨/B5), #16(32ch 측정), #17(GPU 가시성 의존), #18(저장실패 UI), #19(placeholder 풀), Windows D3D11 zero-copy(별도).
