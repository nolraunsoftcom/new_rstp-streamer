# M3 UI 레거시 패리티 계획 (2026-06-13)

대상: `new_viewer`(헥사고날, FFmpeg) UI를 레거시 `../viewer`(단일 MainWindow + **libVLC**)와 **완전 패리티**로 정합.
레퍼런스: `/Users/ieonsang/nolsoft/ziilab/viewer/src` — MainWindow.cpp(2689), ConnectionDialog, StatusBar, Style.h, ChannelInfoDialog, **VlcWidget(1306, libVLC 의존부)**.

## 확정된 범위 결정
1. **정합 목표: 레거시 완전 패리티** — P1~P4 전부.
2. **저장 경로: `~/.ziilab/{snapshots,recordings}`** — 레거시와 동일(분리 디렉토리). `RecordingPaths` 변경.
3. **relay(MediaMTX): UI만 선반영** — 다이얼로그에 체크박스/path 필드(비활성+툴팁"M4 제공"), 백엔드 M4.
4. **P4는 테스트 가능·안정적 세부 단계(P4a~P4g)로 분할** — 각 단계 독립 머지 가능.
5. **libVLC 공짜 기능은 FFmpeg 경로에서 커스텀** — 단, 커스텀이 성능을 해치면 **스킵 가능(분석은 본 문서에 기록)**. → §"libVLC→FFmpeg 커스텀 매핑".

---

## 차이 요약 (전수)
| 영역 | 레거시 | 현재 | 단계 |
|---|---|---|---|
| 우측 패널 폭 | 280px | 320px | P1 |
| 상태바 높이 | 30px | 24px | P1 |
| 툴버튼 호버색 | #0f62fe | #0078d4 | P1 |
| 파일 탭 이름 | "스냅샷/녹화" | "파일" | P1 |
| 채널 다이얼로그 외형 | 폭520·그룹박스·placeholder·확인/취소 | 단순폼 | P1 |
| 파일 타입 토글/폴더열기/우클릭/64×48 | 있음 | 없음/↺/48px | P2 |
| 저장 경로 | ~/.ziilab/{snapshots,recordings} | Movies/new_viewer 단일 | P2 |
| 토스트 시스템 | 리치 오버레이(3.5s) | 없음(QMessageBox) | P3 |
| 상태바 지표(Bitrate/FPS/Dropped) | libVLC stats | packetsPerSec만 | P4a~P4c |
| REC 뱃지 Starting(노랑) | 있음 | 빨강 단일 | P4d |
| 전체화면 탭 | 더블클릭→독립 탭 | "전체"만 | P4e |
| ChannelInfoDialog | 있음 | 없음 | P4f |
| relay 옵션 UI | 있음 | 없음 | P4g |

---

## libVLC → FFmpeg 커스텀 매핑 (핵심 분석)

레거시가 libVLC로 "공짜"로 얻던 것들을 new_viewer는 직접 구현해야 한다. 각 항목의 **원천 API / 커스텀 방법 / 성능 영향 / 권고**:

### 1. 채널 지표 — `libvlc_media_get_stats` → `libvlc_media_stats_t`
| 레거시 필드 | libVLC 원천 | FFmpeg 커스텀 | 성능 | 권고 |
|---|---|---|---|---|
| Bitrate | `f_demux_bitrate`(bytes/µs×8000) | `FfmpegStreamSource` demux 루프에서 **초당 AVPacket 크기 합산**(`pkt->size`) | 무시 가능(덧셈 1회/패킷) | **구현** |
| FPS | `i_displayed_pictures` 델타/경과 | 이미 있는 `onFramePresented`(표시) 또는 `onFrameDecoded`(디코드) **카운트/초** | 무시 가능(카운터) | **구현**(표시 기준 권장) |
| Dropped | `i_lost_pictures` | FFmpeg엔 직접 대응 **없음** — 의미 정의 필요(아래) | 카운터라 무시 가능 | **구현하되 의미 합의 후**(P4c 분리) |
| Corrupted/Discont. | `i_demux_corrupted/discontinuity` | `AVERROR_INVALIDDATA` 수신 카운트(이미 send_packet에서 감지) | 무시 가능 | 선택(상태바엔 미표시, 진단용) |

> **"Dropped" 의미 정의(P4c 선결)**: libVLC의 lost_pictures는 "디코드했으나 표시 못 한 프레임"이다. FFmpeg 경로의 후보 — (a) 디코드 실패(`recvRc<0`/`INVALIDDATA`) 프레임 수, (b) 렌더러가 늦어 건너뛴 슬롯 프레임(슬롯은 최신 1장만 유지 → publish됐으나 소비 전 덮인 수), (c) 녹화 큐 드랍(`m_droppedPackets`, 별개). **권장 정의: (a)+(b)** = "디코드 실패 + 표시 누락". 슬롯 덮어쓰기 카운터를 `LatestSurfaceSlot`에 추가하면 (b) 측정 가능(publish 시 직전 미소비 seq면 ++drop). 성능 영향 없음(원자 카운터).

### 2. 녹화 — `:sout=#std{access=file,mux=mkv}` + 별도 record player
- **레거시**: 두 번째 libVLC 미디어 플레이어를 `:sout`로 띄워 MKV mux. → **RTSP 연결이 2개**(표시용 1 + 녹화용 1) = 장비 세션·대역폭 2배 부담.
- **new_viewer(M3 완료)**: 단일 demux에서 **패킷 fan-out**(같은 AVPacket을 디코더+레코더에 분배) → 연결 1개, "화면=녹화 바이트 일치". **이미 커스텀 완료이며 레거시보다 우월** — 추가 작업 불필요.

### 3. 스냅샷 — `libvlc_video_take_snapshot`
- **레거시**: VLC가 표시 중인 프레임을 PNG로. **new_viewer(M3 완료)**: 슬롯 최신 RGBA(오버레이 없는 원본)→`PngSnapshotWriter`. 이미 커스텀 완료. (차이: 레거시는 표시 프레임=오버레이 포함 가능, new는 순수 디코드 프레임 — 사양상 new가 더 깨끗.)

### 4. 렌더링/전체화면 — `libvlc_media_player_set_nsobject/set_hwnd`
- **레거시**: VLC가 네이티브 윈도우에 직접 렌더. 전체화면 탭 = 플레이어를 새 위젯에 부착(또는 새 플레이어).
- **new_viewer**: `RhiVideoRenderer`/`SwVideoRenderer`가 `LatestSurfaceSlot`(채널별, `IFrameSurfaceRegistry`로 id 조회)을 폴링. **전체화면 탭은 두 번째 `VideoTileWidget`이 같은 슬롯을 폴링하면 됨 → 추가 디코드 0**. 아키텍처상 깔끔(슬롯 공유). 성능 영향 미미(같은 RGBA를 한 번 더 텍스처 업로드하는 정도 — 전체화면 1개라 무시 가능).

**결론**: 성능 문제로 스킵해야 할 항목 **없음**. 지표 카운터·슬롯 공유 모두 저비용. 유일한 비용 항목은 전체화면 탭의 추가 텍스처 업로드 1개분(무시 가능). "dropped"는 성능이 아니라 **의미 정의**가 선결 과제.

---

## 단계별 실행 계획

### P1 — 스타일/문자열 정합 (반나절, 저위험)
파일: `src/ui/shell/MainWindow.cpp`, `src/ui/channels/ChannelDialog.cpp`, `src/ui/grid/GridView.cpp`, 신설 `src/ui/common/Style.h`
- 우측 패널 320→**280**, 상태바 24→**30**, 툴버튼 호버색 `#0f62fe` 정합.
- 파일 탭 이름 "파일"→**"스냅샷/녹화"**.
- 다이얼로그: 최소폭 **520**, placeholder `rtsp://<카메라 IP>:8900/live`, 버튼 **"확인/취소"**, 그룹박스("채널 정보"/"옵션").
- **테스트**: 빌드 + 기존 단위/통합 무회귀. (픽셀은 화면 기록 권한 시 육안.)

### P2 — 파일 탭 조회 + 저장 경로 (1일)
파일: `src/ui/panels/FilePanel.{h,cpp}`, `src/infra/persist/RecordingPaths.h`, `src/ui/main.cpp`
- 타입 토글(스냅샷/녹화 라디오, `QButtonGroup`), 폴더 열기 📁, 우클릭 메뉴(열기/Finder/삭제), 아이콘 64×48.
- `RecordingPaths`: `~/.ziilab/recordings`, `~/.ziilab/snapshots` 분리. `NV_RECORD_DIR`/스냅샷 배선 동기화. watcher 2개 디렉토리.
- **테스트**: RecordingPaths 단위테스트(경로 형식·분리), FilePanel 타입필터 로직. 통합테스트 `NV_RECORD_DIR` 오버라이드 유지 확인.

### P3 — 토스트 시스템 (1일)
파일: 신규 `src/ui/shell/Toast.{h,cpp}`, `src/ui/main.cpp` 배선
- 레거시 `showToast(title, detail, level, primary/secondary, timeout=3500)` 오버레이(QFrame+닫기×+액션, 우하단 `positionToast`).
- 트리거(control→UI queued): 스냅샷 저장됨 / 녹화 자동 저장됨 / 녹화 실패. (전체화면 실패는 P4e, MediaMTX는 M4.)
- **테스트**: 토스트 표시/타임아웃/수동닫기 로직 단위화(위젯 수명). 성공/실패 경로 배선 스모크.

### P4 — 완전 패리티 (세부 단계, 각 독립 머지)

#### P4a — 채널 지표 데이터 경로: Bitrate/FPS (1일)
- `FfmpegStreamSource`: 초당 패킷 바이트 합산(bitrate), `onFramePresented` 카운트(fps). atomic 누적 → 1초 tick에서 rate 산출.
- `ChannelSnapshot`에 `bitrateKbps`, `fps` 필드 추가 → `ChannelController` 집계.
- **UI 없음(데이터만)**. **테스트**: 가짜 소스로 패킷/프레임 주입 → bitrate/fps 단위테스트. 순수 additive라 무회귀.

#### P4b — 상태바 지표 표시 (반나절)
- `MainWindow::updateStatusBar`를 레거시 포맷으로: `Connected: x/y | Bitrate: z Mbps | FPS: w | Dropped: n`. 기존 "⚠ 전 채널 끊김" 경보는 **보존**(레거시 대비 개선).
- **테스트**: 집계 텍스트 포맷 단위테스트.

#### P4c — Dropped 지표 (반나절, 의미 합의 선결)
- 정의 확정(권장: 디코드 실패 + 슬롯 표시누락). `LatestSurfaceSlot`에 덮어쓰기 드랍 카운터, 디코드 실패 카운터 → `ChannelSnapshot`.
- **테스트**: 손상 패킷 주입 시 드랍 카운트 증가 단위테스트.

#### P4d — REC 뱃지 Starting(노랑) 2단계 (반나절)
- "요청됨(시작 대기)" vs "실제 녹화중" 구분 → 뱃지 노랑/빨강. `RecordingController` 상태/sink.isRecording 조합 또는 `RecordingState`에 Starting 표면화(부채 #29 Stopping 미사용과 함께 정리).
- **테스트**: 상태 전이 단위테스트(Idle→Starting→Recording→Idle).

#### P4e — 전체화면 탭 (1~2일)
- GridView 타일 더블클릭 시그널 → `MainWindow`가 `videoTabs`에 채널 독립 탭 추가(닫기 가능). 두 번째 `VideoTileWidget`이 **같은 슬롯 폴링**(추가 디코드 0).
- 탭 닫기/채널 삭제 시 정리. 더블클릭 중복 방지.
- **테스트**: 탭 add/close 수명, 슬롯 재사용(디코드 카운트 불변) 검증. 실패 시 토스트(P3).

#### P4f — ChannelInfoDialog (1일)
- 채널 상세(이름/URL/상태/지표 P4a) 팝업 이식. 비모달, 채널별 1개(`QPointer` 맵).
- **테스트**: 스냅샷→다이얼로그 표시 갱신 로직.

#### P4g — relay 옵션 UI 선반영 (반나절)
- 다이얼로그에 "MediaMTX relay 사용" 체크박스 + "Relay path:" 필드. **비활성 + 툴팁 "M4 제공"**. 저장/검증 미배선(외형만).
- **테스트**: 다이얼로그 빌드/표시 스모크.

---

## 리스크/주의
- **P4c "dropped" 의미**가 유일한 선결 합의 사항(성능 아닌 정의 문제).
- 저장 경로 변경(P2)은 통합/배터리 테스트의 `NV_RECORD_DIR` 가정과 충돌 가능 → 오버라이드 경로 우선순위 확인.
- relay UI만 노출 시 "동작 안 함" 혼동 → 비활성+툴팁 필수.
- 지표 데이터 경로(P4a)는 domain/app/infra를 건드리는 유일한 구간 → architect 검증 THOROUGH.
- 각 단계 designer/executor 위임 → architect 검증 → 머지. UI 픽셀 검증은 화면 기록 권한 필요(M1~M3 수용 보류 항목과 동일).

## 권장 진행 순서
P1 → P2 → P3 (1차 PR, 외형 체감·저위험) → P4a → P4b → P4c → P4d → P4e → P4f → P4g.
지표 3종(P4a~c)을 묶고, 전체화면 탭(P4e)은 별도 설계 리뷰 후 착수.
```
의존: P4b ← P4a, P4c (지표 먼저). P4f ← P4a(지표 표시). 나머지 독립.
```
