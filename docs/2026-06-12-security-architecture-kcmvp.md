# 보안 아키텍처: KCMVP 기반 영상 암호화 (VOXL2 → new_viewer)

- 작성일: 2026-06-12
- 상태: 초안 (아키텍처 방향 논의 정리, 구현 착수 전)
- 관련 문서: `2026-06-12-new-viewer-design.md`, `issues/2026-06-12-ghost-session-*.md`
- 관련 자료: `ziilab/펜타(KCMVP)/` (펜타 CIS-CC 4.0 SDK), `양자암호(proxyRTSP) 사용자 메뉴얼` (선행 제품 참고)

## 1. 배경과 전제

VOXL2 Mini 기반 휴대형 카메라(1회 최대 ~12h 운용)를 new_viewer(Qt6 + FFmpeg, GCS PC)로
관제하는 시스템. 군납 대상이므로 보안적합성 검증을 전제로 설계한다.

- 영상 암호화는 **KCMVP 검증필 암호모듈(펜타시큐리티 CIS-CC)** 사용 확정.
- 장비 측 streamer는 `voxl/voxl-ziilab-streamer` 포크로 운영 중 (유령세션 완화 배포됨).
- MediaMTX는 GCS(viewer 호스트)의 OS 서비스, `sourceOnDemand: no` 상시연결.
- RTSP 양 레그 TCP 강제 (기존 설계 결정).

## 2. 신뢰 경계

```
[VOXL2 Mini]──────── 무선/네트워크 ────────[GCS PC]
 voxl-streamer            ▲                 MediaMTX (localhost) ── new_viewer
 (H.264 인코딩)            │                                          └─ 녹화 MKV
                    실제 위협 구간
```

| 구간 | 위협 | 판단 |
|---|---|---|
| 장비 → GCS 무선 | 도청·변조·재전송·위장 장비/GCS | **암호화 1순위 대상** |
| MediaMTX ↔ viewer | localhost지만 MediaMTX는 Go 표준 암호(비검증) | 평문이 보이면 안 됨 |
| 녹화 MKV | GCS 분실·탈취 | at-rest 암호화 필요 |

**핵심 결론: TLS/SRTP 등 표준 전송 암호화는 부적합.**
MediaMTX가 TLS를 종단하는 순간 (a) 평문이 비검증 구간에 노출되고
(b) 암호 연산이 검증필 모듈 밖(Go crypto)에서 수행되기 때문.
→ **장비에서 페이로드 자체를 KCMVP 모듈로 암호화하고, MediaMTX는 암호문을 그대로
중계하며, viewer가 복호화하는 종단간(E2E) 페이로드 암호화**로 간다.
부수효과: MediaMTX가 신뢰 경계 밖으로 완전히 빠진다 (평문을 영원히 못 봄).

## 3. 기본 보안요건 (군납 영상관제)

### 제도/인증

- **KCMVP 검증필 암호모듈 사용** — 국가·공공(군 포함) 도입 시 암호 기능은 검증필 모듈로 수행.
- **보안적합성 검증** (국정원/국방부) — KCMVP는 그중 암호 요건 하나. 인증·키관리·감사·하드닝 등
  시스템 전체가 심사 대상.
- 사업 형태에 따라 CC 인증 / 국방 시험평가 추가 가능 → 발주처 요구사항 문서에서 조기 확인.

### 기술 체크리스트

1. **기밀성**: 영상 + 제어 채널(RTSP 시그널링, MediaMTX Control API) 암호화.
   영상만 암호화하고 제어를 평문으로 두면 심사에서 걸림.
2. **상호 인증**: 장비 ↔ GCS (위장 장비/위장 GCS 차단) + 운용자 로그인·권한.
3. **무결성·재전송 방지**: 패킷 단위 인증 태그(AEAD) + 시퀀스 번호.
4. **키관리 수명주기**: 생성 → 주입 → 세션키 파생 → 갱신 → 폐기(zeroize).
   휴대 장비라 **분실 시나리오**가 특히 중요.
5. **플랫폼 하드닝(VOXL2)**: 운용 모드에서 **ADB·SSH 비활성**(현재 벤치는 ADB 사용 중 —
   양산 프로파일에서 반드시 차단), 불필요 서비스 제거, QRB5165 secure boot/펌웨어 서명 검토.
6. **저장 데이터 보호**: 녹화 MKV 암호화 (§4.4에서 구조적으로 해결).
7. **감사 로그**: 접속/스트림 시작·종료/키 갱신 이벤트 + 시각 동기.
8. **가용성**: 재밍·단절 시 동작은 M1 상태머신 영역. 보안 기능이 재연결 견고성을
   해치지 않을 것 (§6 키관리와 직결).

## 4. 영상 암호화 설계: E2E NAL 페이로드 암호화

### 4.1 계층 선택

| 방식 | 평가 |
|---|---|
| (a) 전송 계층 (TLS/SRTP) | 탈락 — MediaMTX 종단 + 비검증 암호 구현 |
| **(b) NAL 페이로드 암호화** | **채택** — RTP/RTSP 구조 유지, 기존 경로 그대로 통과 |
| (c) 전용 프로토콜 | 가장 깨끗하나 MediaMTX·FFmpeg 경로 전부 재작업 |

### 4.2 동작 원리

```
[VOXL2]
 H.264 인코더 → NAL 단위로:
   - NAL 헤더(1바이트) + 보안헤더(버전|키ID|IV/카운터|시퀀스) = 평문
   - NAL 페이로드 = KCMVP 모듈로 AEAD 암호화 (+ 인증태그)
 → RTP 패킷타이저(FU-A 분할 포함, 암호문은 불투명 데이터)
 → RTSP/TCP → MediaMTX (구조만 보고 중계)

[new_viewer]
 RTSP 수신 → FFmpeg demux (AVPacket = 암호화된 NAL들)
 → 복호화 스테이지 (KCMVP 모듈) → 디코더/녹화 fan-out
```

### 4.3 왜 NAL 단위인가

- **재패킷타이징에 안전**: MediaMTX는 RTSP 중계 시 depacketize→repacketize를 수행할 수 있다.
  RTP 패킷 단위 암호화는 패킷 경계가 바뀌면 깨지지만, NAL 단위 암호화는 FU-A 재조립 후에도
  바이트가 동일하므로 살아남는다.
- **NAL 헤더 바이트와 SPS/PPS는 평문 유지** — MediaMTX/FFmpeg가 스트림 타입을 인식하는 데 필요.
  SPS/PPS는 SDP(`sprop-parameter-sets`)로도 전달됨. 노출 정보는 해상도·프로파일 수준
  (일반적으로 수용 가능하나 발주처 요구 확인 필요 — §8).
- 암호문 NAL 형식(안): `[NAL hdr 1B][보안헤더: ver|keyID|IV카운터|seq][ciphertext][tag 16B]`
  — NAL당 오버헤드 ~30–40B, 비트레이트 대비 무시 가능.

### 4.4 녹화 at-rest 보호가 공짜

복호화 **전** 패킷을 MKV에 쓰면 저장 영상이 자동으로 암호화 상태 → at-rest 요건을
별도 구현 없이 충족. 트레이드오프: 재생 시에도 복호화 스테이지 필요.
(대안: 복호화 후 파일키로 재암호화 — 표준 플레이어 호환이 필요해지면 검토)

### 4.5 성능

비트레이트 2–10Mbps = 0.25–1.25MB/s. LEA/ARIA 소프트웨어 구현도 Cortex-A에서 수백 MB/s
(펜타 제공 속도자료: `penta_symmetric_cycle speed test.xlsx`).
viewer 20채널 동시(M2 성능보장선)도 ~20MB/s 복호화로 CPU 베이스라인(101.8%) 대비 미미.
지연은 패킷당 µs 단위.

## 5. 장비 측 구현: crypto-proxy (분리형)

### 5.1 streamer 수정 대신 별도 데몬 — 결정

streamer 내부 암호화(인코더~payloader 사이 커스텀 엘리먼트)는 인코더 wedge·유령세션
버그가 사는 코드를 직접 건드린다. 분리하면:

- **KCMVP 모듈 경계가 프로세스 단위로 명확** — 심사 시 "암호 기능은 이 데몬 안"으로 선 긋기 가능
- **포크 발산 최소화** — upstream 리베이스 부담이 유령세션 완화 + bind 주소 수준으로 유지
- **장애 도메인 분리** — proxy 죽어도 인코더 무사, systemd 개별 재시작

### 5.2 토폴로지

```
[VOXL2 Mini]
  카메라 → 인코더 → voxl-streamer (RTSP, 127.0.0.1:8554 전용)
                        ↑ localhost RTSP (평문, 장비 내부)
                   crypto-proxy ── NAL 페이로드 암호화 (KCMVP 모듈)
                        │ 외부 노출 포트는 이것 하나 (예: 8555, 암호문)
════════ 무선 ════════
[GCS PC]
  MediaMTX (proxy를 source로 pull, 암호문 중계)
      ↑ localhost
  new_viewer (demux → 복호화 스테이지 → 디코더/녹화)
```

- 장비에 직접 붙는 것은 앱이 아니라 **GCS의 MediaMTX**. 앱은 지금처럼 localhost MediaMTX 사용.
- **voxl-streamer localhost 잠금**: 현재 포크 `main.c:649` 부근은 `service`(포트)만 설정 →
  0.0.0.0 바인딩. 한 줄 추가:
  ```c
  g_object_set(context.rtsp_server, "address", "127.0.0.1", NULL);
  ```
  심층 방어로 iptables에서 8554 외부 차단 병행.

### 5.3 결합 방식: 분리형 채택 (투명 패스스루 기각)

| 방식 | 동작 | 판단 |
|---|---|---|
| 투명 패스스루 | 외부 연결마다 localhost로 1:1 릴레이 + interleaved RTP만 암호화 | 기각 — 무선 끊김·배터리 교체마다 streamer 세션이 같이 죽음 (유령세션/wedge 자극) |
| **분리형** | proxy가 streamer의 **유일한 고정 클라이언트**(localhost 세션 상시 유지), 외부 레그는 독립적으로 끊김/붙음 | **채택** — 무선이 출렁여도 streamer는 안정적 로컬 클라이언트 1개만 봄 |

분리형은 MediaMTX가 하던 "장비 보호막" 역할이 장비 안으로 들어오는 구조로,
`sourceOnDemand: no` 상시연결 철학과 일치.

### 5.4 구현 스케치 (GStreamer 재사용)

VOXL2에 이미 있고 팀이 아는 스택:

```
rtspsrc(localhost:8554) ! rtph264depay
  ! [암호화 transform: NAL 헤더 평문, 페이로드 AEAD — 펜타 CIS-CC 호출]
  ! rtph264pay ! gst-rtsp-server(0.0.0.0:8555)
```

`rtph264depay`가 FU-A 재조립까지 끝낸 완전한 NAL 버퍼를 주므로 transform이 NAL 단위로
동작하고, `rtph264pay`가 암호문을 다시 분할한다. §4의 설계와 정확히 맞물림.

### 5.5 트레이드오프: 전력

분리형은 proxy가 로컬 세션을 상시 유지 → GCS 미접속 시에도 인코더 가동.
12h 배터리 운용에서 전력이 문제면 "다운스트림 있을 때만 로컬 레그 연결" idle 정책을
추가할 수 있으나 churn 차단 효과가 줄어드는 맞교환.
**1차는 상시 유지로 가고 전력 실측 후 판단.**

### 5.6 장기 레버 (지금 안 건드림)

분리형 proxy가 보호막 역할을 장비 쪽에서 수행하면, 장기적으로 GCS MediaMTX의 역할
(외부 배포·진단 API)을 재평가할 여지가 생김. M-Sec 이후의 단순화 후보로만 기록.

## 6. 키관리 설계

운용 형태(사람 휴대, 1회 ≤12h)에 맞춰 2단계:

- **1차 (단순)**: 출동 전 점검 단계에서 장비별 마스터키(PSK) 주입 →
  세션 시작 시 KDF(모듈 내)로 세션키 파생. 키 주입 도구만 필요, PKI 불필요.
- **2차 (확장)**: 장비 인증서 + ECDH 키교환 + 서명 상호인증.
  장비 대수 증가 또는 발주처 PKI 요구 시 전환. (SDK에 `KeyAgreement.h`,
  `DigitalSignature.h` 이미 포함)

**GCM IV 관리 (치명 사항)**: 동일 키로 IV 재사용 절대 금지.
"세션키 + 96bit IV(salt ‖ 단조 카운터)" 구조로 하되, **전원 차단·배터리 교체가 정상
흐름**이므로 재부팅 시 카운터 재사용이 없도록 **재시작마다 새 세션키 파생**을 원칙으로 한다.

## 7. 펜타 CIS-CC SDK 현황 (확보 자료 검토 결과)

`ziilab/펜타(KCMVP)/`에 CIS-CC 4.0 SDK 확보됨:

| 항목 | 내용 |
|---|---|
| 플랫폼 | **Embedded Linux 4.19 armv8a** (VOXL2/QRB5165 커널 4.19와 일치) + **Windows 10** (viewer 타깃) — 필요한 두 플랫폼 모두 확보 |
| 알고리즘/모드 | ARIA/LEA/SEED/HIGHT × ECB/CBC/CFB/OFB/CTR/**GCM** (BlockCipher.h 확인) |
| 부가 기능 | HMAC(SHA-2/LSH), KDF, RNG, 키합의(ECDH), 전자서명 헤더 포함 |
| 샘플 | `CIS-CC_4.0_TEST_ARIA.cpp`, `CIS-CC_4.0_TEST_LEA.cpp`, `LEA_CTR_TEST.cpp`, `sample/api_test.c` |
| 속도자료 | `penta_symmetric_cycle speed test.xlsx` |

펜타 측 가이드(`개발방법.txt`): 영상은 원문=암호문 길이인 모드(CFB/CTR) 권장, CTR이 최속.

**우리 판단**: CTR 단독은 무결성이 없어 §3-3 요건을 못 채움. 선택지는
**(1순위) GCM** (AEAD 한 방, SDK에 정의 존재) 또는
**(2순위) CTR + HMAC** (Encrypt-then-MAC, 둘 다 모듈 내 수행).
→ GCM이 **검증서의 검증 범위**에 포함되는지 펜타에 확인 후 확정 (§8-2).

## 8. 미결 확인사항 (외부 변수)

1. **발주처 보안요구 문서**: 보안적합성 검증 범위, SPS/PPS·NAL 헤더 평문 허용 여부,
   at-rest 요건, PKI 요구 여부.
2. **펜타 검증서 확인**: (a) 검증서의 **운용 환경 목록**에 우리 플랫폼(armv8a Linux,
   Windows)이 포함되는지 — SDK 존재 ≠ 검증 환경 포함. (b) **GCM 모드가 검증 범위**인지
   (검증필 목록상 모드 제한 가능). (c) Windows 10 SDK의 Windows 11 인정 여부.
3. **선행 제품 검토**: `양자암호(proxyRTSP)` 메뉴얼·`ZL-Q20_pRTSP 설치가이드` —
   유사 구조(RTSP 암호 프록시) 선행 사례로 프레이밍·키관리 방식 참고 가치.

## 9. 진행 순서 (제안)

1. **지금**: §8 외부 변수 확인 (발주처 요구 + 펜타 검증 범위 문의).
2. **M2 진행 중 (비용 ~0)**: viewer 패킷 파이프라인에 복호화 스테이지 **인터페이스만**
   정의 (현재는 pass-through). M2를 막지 않음.
3. **M-Sec (별도 마일스톤, M2b 이후)**:
   - crypto-proxy 데몬 (장비, GStreamer 기반, 분리형)
   - voxl-streamer localhost 잠금 (포크 한 줄) + iptables
   - viewer 복호화 스테이지 실구현 + 암호화 MKV 재생
   - 키 주입 도구 (1차 PSK 방식)
   - 제어 채널 보호, 감사 로그
   - 장비 하드닝 (ADB/SSH 운용 프로파일 차단)

## 결정 기록

| 결정 | 내용 | 근거 |
|---|---|---|
| D-S1 | E2E NAL 페이로드 암호화 (전송계층 TLS/SRTP 기각) | MediaMTX 비검증 구간 평문 노출 + 모듈 외 암호연산 |
| D-S2 | 장비 측은 streamer 수정 대신 **crypto-proxy 데몬** | 모듈 경계 명확, 포크 발산 최소, 장애 도메인 분리 |
| D-S3 | proxy는 **분리형** (투명 패스스루 기각) | 무선 churn으로부터 streamer 세션 절연 (유령세션/wedge) |
| D-S4 | 알고리즘 1순위 GCM, 2순위 CTR+HMAC | AEAD 요건, 검증 범위 확인 후 확정 |
| D-S5 | 재시작마다 새 세션키 파생 | GCM IV 재사용 차단 (배터리 교체가 정상 흐름) |
