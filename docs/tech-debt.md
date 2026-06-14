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
| 8 | 슬롯 레지스트리 무한 증가 | **해소 — destroySource가 slot->clear()로 GPU/RGBA 자원 반납(슬롯 객체는 폴링 안전 위해 보존). M2 안정성 리팩토링** | M2 안정성 리팩토링 |
| 9 | soak.csv 무한 append·상대경로 | 회전/상한 없음 | M2b ride-along (SoakLogger 추출) |
| 10 | URL 평문 표시 | 미수행 — M3로 재지정. rtsp://user:pass@ 형태를 화면·로그에서 마스킹 | M3 |
| 11 | 패널/컬럼 선택 미영속 | QSettings 저장 안 함 — 재시작 리셋 | M3 |

## M2a 최종 리뷰(2026-06-12) 잔여 — 머지 승인됨, 비차단

| # | 항목 | 내용 | 해소 |
|---|---|---|---|
| 12 | CompositeLogger::setCallback 스레드 안전성 | log()와 setCallback 간 미동기화 — 현재 teardown 순서(drain 후 호출)로 레이스 창 near-zero | **해소(B3/M2b)**: SoakLogger 추출 시 atomic 처리 적용 확인 |
| 13 | GridView m_tiles QString 키 | relayout 핫패스에서 fromStdString 반복 — 32ch에서 무영향 | **해소(B5/M2b)**: RhiVideoRenderer/SwVideoRenderer 도입 시 std::string 키로 전환 |
| 14 | 상태→한글/색 매핑 중복 | GridView·ChannelListPanel 2곳 복제 | M2b ride-along (StatusDisplay 추출) |
| — | 전 채널 다운 경보 | 상태바 "연결 N / 전체 M" 표시, M>0 && N==0 시 빨강+굵게+경보 문구 | **구현됨(M2a closeout)**: MainWindow.cpp updateStatusBar() |

## M2b 실측 후 (2026-06-12)

| # | 항목 | 내용 | 해소 |
|---|---|---|---|
| 15 | HW 디코드 GPU→CPU 왕복 | VideoToolbox 디코드 후 av_hwframe_transfer_data로 CPU 복사 + sws RGBA + 텍스처 재업로드(동반-rgba). 진짜 zero-copy(CVPixelBuffer→CVMetalTextureCache) 미적용 — 20ch CPU의 주 오버헤드 | M2b 후속 성능 작업 (가장 큰 CPU 레버) |
| 16 | 32ch 성능 미측정 | 시뮬 32 퍼블리셔가 측정 머신 포화 — 별도 부하 발생원/세션 필요 | 후속 (분리 측정) |
| 17 | GPU framePainted 가시성 의존 | RhiVideoRenderer.render()가 compositing 시에만 호출 → 오프스크린/최소화 창은 표시단계 미확정(구 SW paintEvent도 동일 특성, 회귀 아님) | M3 (필요 시 보정) |
| 18 | 저장 실패 UI 미표시 | save 실패가 로그 한 줄뿐 — 상태바/다이얼로그 미반영 | **부분(M3)**: 녹화 시작 실패는 `RecordingController`가 Idle 유지 + Warn 로그 + 옵저버로 UI에 비-녹화 상태 통지(● 버튼 빨강 미점등으로 시작 실패 가시화). 단 채널 설정 save 실패(`ChannelManager::persist`)·녹화 시작 실패의 **명시적 토스트/상태바 알림**은 미구현 — M4 UI 알림으로 이월 |
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

## M2 안정성 리팩토링 (2026-06-13)

| # | 항목 | 내용 | 해소 |
|---|---|---|---|
| 24 | 순차 다채널 종료 누적 지연 | close() 타임드 join 상한 5s → 20채널 순차 종료 시 최대 ~20s 소요. 병렬 disconnect는 teardown 순서 민감 — 보류. 실장비 wedge 발생 시 개선 우선순위 재검토 | 보류 (M3 이후) |

남은 부채: #12(CompositeLogger, 해소됨/B3), #13(QString키, 해소됨/B5), #16(32ch 측정), #17(GPU 가시성 의존), #18(저장실패 UI — M3 부분 해소, 명시 알림 M4 이월), #19(placeholder 풀), #24(순차 종료 누적 지연), Windows D3D11 zero-copy(별도).

## M3 (2026-06-13, 녹화/스냅샷)

| # | 항목 | 내용 | 해소 시점 |
|---|---|---|---|
| 25 | 녹화 시작 실패 명시 알림 | 시작 실패가 Warn 로그 + ●버튼 미점등뿐 — 토스트/상태바 명시 알림 미구현(#18과 묶임) | M4 (UI 알림) |
| 26 | 통합테스트 픽스처 GOP 의존 | 픽스처를 1초 키프레임으로 안정화(중간 합류 flaky 제거) — 단, 통합테스트가 여전히 고정 sleep 사용(#3)이라 매우 느린 CI에선 잔여 flaky 소지 | CI 도입 시(#3과 묶음) |
| 27 | 주기 롤오버(maxDuration) tick 미배선 | `RecordingController::tick`(10분 세그먼트 롤오버)이 main 루프에 미배선 — onReconnect 분리만 배선됨. 장시간 단일 세그먼트가 커질 수 있음 | M4 (장시간 녹화 운영) |

## M3 결함 수정 2차 (2026-06-13, 녹화 리뷰 D1~D5)

해소:

| 결함 | 내용 | 수정 |
|---|---|---|
| D1 | stop→start 래치 경합으로 세그먼트 롤오버 실패 | **해소** — `FfmpegStreamSource::startRecording`이 녹화 중에도 거절하지 않고 새 경로를 pending으로 수락. 디코드 스레드 `serviceRecording`이 `pendingPath != currentPath`를 감지해 finish→재start(세그먼트 전환을 디코드 스레드 단일 소유). control 스레드 래치 경합 제거. 통합테스트 #157(stop·유예 없는 즉시 전환) 추가 |
| D2 | 같은 초 파일명 충돌 → 직전 세그먼트 truncate | **해소** — `RecordingController::makePath`에 단조 시퀀스(`_NNN`), `RecordingPaths::makePath`에 밀리초(`_zzz`)+프로세스 단조 카운터 추가. 단위테스트 2종 추가 |
| D3 | REC 표시 불일치(디코드 스레드 start 실패 시 컨트롤러 영구 Recording) | **해소** — `tick()`에서 `sink.isRecording`과 컨트롤러 상태 대조, Recording인데 sink 비녹화면(유예 3초 후) Idle 수렴 + 경고 로그 + 옵저버 통지. 단위테스트 2종 추가 |
| D4 | 스냅샷 PNG 인코딩이 control 스레드 블로킹 | **해소** — `ChannelSourceFactory::snapshot`이 RGBA 복사(latest)까지만 control 스레드, PNG 인코딩·저장은 detach 워커 스레드로 분리 |
| D5 | FilePanel 썸네일 풀 디코드로 UI 멈춤 | **해소** — `QImageReader::setScaledSize`로 축소 디코드 + 경로+mtime 캐시(`QHash`) 재사용 |
| (debt) | writePacket 오류 후 패킷마다 재시도+로그 스팸 | **해소** — `m_errored`면 조기 반환 |

남은 부채(이번에 보류):

| # | 항목 | 내용 | 해소 시점 |
|---|---|---|---|
| 28 | 음수 dts (avoid_negative_ts 의존) | FfmpegRecorder가 첫 키프레임 pts를 0으로 당기지만 B프레임 dts가 음수일 수 있어 muxer 기본(avoid_negative_ts=auto)에 의존. 명시 설정/주석 미정 | M4 |
| 29 | RecordingState::Stopping 미사용 / sanitizeName 중복 | enum의 Stopping 상태 미사용(Idle/Recording만), 채널명 살균 로직이 RecordingController(std)와 RecordingPaths(Qt)에 중복. 정리 보류 | 정리 라운드 |
| (기존 #25) | 디스크 용량 관리 부재 | 회전/상한 없음 | M4 |

## M1~M3 전체 리뷰 잔여(2026-06-13, 전부 Low — 제품 결함 아님, 마지막 정리 라운드에 일괄)

| # | 항목 | 내용 | 해소 시점 |
|---|---|---|---|
| 30 | 부채 #27 장부 오기 | #27이 "`RecordingController::tick` 미배선"이라 적혔으나 실제로는 `395ff34`에서 배선됨(`main.cpp:75` `r->tick()`), 배터리 테스트가 600s maxDuration 롤오버 7개 실증. **#27은 해소됨으로 정정 필요** — 현 장부가 "롤오버 안 됨"으로 오해 유발 | 정리 라운드 |
| 31 | 스냅샷 PNG 비원자적 쓰기 → libpng Read Error | `PngSnapshotWriter::write`가 최종 경로에 직접 저장 → `FilePanel` `QFileSystemWatcher`가 쓰는 중 파일을 디코드해 `libpng error: Read Error`(1h 3회 관찰). 크래시 없이 텍스트 아이콘 폴백이라 무해. temp 파일 쓰기 후 `rename`(원자적 교체)으로 watcher가 완성 파일만 보게 하면 해소 | 정리 라운드 |
| 32 | 통합테스트 병렬 플래키(포트 경합) | MediaMTX 통합테스트들이 동일 포트(8554 등) 공유 → `ctest -j` 병렬 시 간헐 실패(#168 TEARDOWN). 직렬은 12/12 통과. CTest `RESOURCE_LOCK` 또는 테스트별 랜덤 포트로 직렬화 | CI 도입 시(#3과 묶음) |
| 33 | `device-battery.sh` RSS 샘플러 깨짐 | 2035 run의 `rss.log`가 `rss_mb=1` 고정 — `diskfull-test.sh`의 `pgrep -x new_viewer` 수정이 `device-battery.sh`엔 미반영. 실 RSS는 앱 `soak.csv`(209~245MB 평탄)로 확인되어 데이터 손실 없음. 스크립트 일관성 차원 수정 | 정리 라운드 |

## 정리 라운드 (2026-06-14, tech-debt-cleanup 브랜치)

부채 전수 트리아지 후 **적합한 것만** 처리(오버엔지니어링 회피).

### 해소
| # | 처리 | 커밋 |
|---|---|---|
| 31 | 스냅샷 원자적 쓰기 — `PngSnapshotWriter`가 temp 저장 후 `std::rename`(완성 파일만 watcher 노출) → libpng Read Error 제거 | `a8facd8` |
| 28 | 녹화 `avoid_negative_ts=make_zero` 명시 — B프레임 음수 dts를 muxer 기본(auto) 의존 없이 0 시프트 | `a8facd8` |
| 33 | **이미 해소(장부 오기)** — `device-battery.sh`는 이미 `pgrep -x new_viewer` 사용 중(이전 수정 main 머지됨). 병렬세션이 옛 run(2035) 기준으로 기록 | (기존) |
| 27 | **이미 해소(#30 정정 확정)** — `RecordingController::tick`은 main.cpp에 배선됨(`r->tick()`), 배터리·1h·8h 소크에서 600s 롤오버 실증 | (기존) |

### 보류 (사유 — 지금 처리 부적합/오버엔지니어링)
| # | 항목 | 보류 사유 |
|---|---|---|
| 3, 26, 32 | 통합테스트 고정sleep/GOP/포트경합 flaky | **CI 도입 시** — 현재 `-j1` 직렬로 안정 통과, CI 없이는 가치 없음 |
| 16 | 32ch 성능 미측정 | 별도 부하생성 환경 필요. 20ch 스케일은 검증됨 |
| 17 | GPU framePainted 가시성 의존 | 회귀 아님(구 SW와 동일 특성), 실해 없음 |
| 18/25 | 저장/녹화 실패 명시 알림(토스트/상태바) | **UI 작업** — `2026-06-13-m3-ui-parity-plan`과 묶임. 중복 회피, UI 라운드로 |
| 19 | placeholder 풀 미축소 | 경미(유한), 기능 영향 없음 |
| 24 | 순차 다채널 종료 누적 지연 | 실장비 wedge 발생 시 재검토(현재 미발생) |
| 25 | 디스크 용량관리(회전/상한) | D10 백오프가 풀-디스크 안전 처리. 사전관리/회전은 큰 작업 → 운영 요구 시 |
| 29 | Stopping enum/sanitizeName 중복 | 순수 미관·기능가치 0 → 오버엔지니어링 회피, 보류 |
