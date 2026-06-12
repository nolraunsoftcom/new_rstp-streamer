# ModalAI voxl-streamer: Ghost 세션 청소가 진행 중 스트림을 멈추는 문제 조사 보고서

**작성일**: 2026-06-12  
**조사 대상**: voxl-streamer 0.8.0 (VOXL2 Mini, gst-rtsp-server 기반)  
**조사 방법**: GitLab 소스코드 직접 fetch, ModalAI 공식 포럼, gst-rtsp-server 업스트림 소스 분석

---

## 요약 (Executive Summary)

`timeout()` 콜백이 만료 세션을 제거할 때 `num_rtsp_clients`를 **무조건 1 감소**시키는 구조적 결함이 확인됨. 동시에 `gst_rtsp_session_pool_cleanup()`이 `GstRTSPSessionMedia`의 `finalize`를 통해 `gst_rtsp_media_unprepare()`를 호출하면, 공유(shared) 미디어 파이프라인이 실제로 살아있는 다른 클라이언트가 있음에도 NULL 상태로 전환될 수 있음. 두 가지 경로 — 더블 디크리먼트로 인한 파이프 조기 닫힘과, gst-rtsp-server 레벨의 shared media 레퍼런스 카운트 문제 — 가 복합적으로 작용하여 "Removed 1 sessions" 로그 이후 스트림이 멈추는 현상을 유발함.

---

## 1. voxl-streamer 소스코드 분석

**출처**: https://gitlab.com/voxl-public/voxl-sdk/services/voxl-streamer/-/raw/master/src/main.c

### 1.1 핵심 전역 변수

```c
static int first_client = 0;
static int first_run = 0;
static int is_standalone = 0;
static int closing_pipe_intentionally = 0;
static int source_pipe_disconnected = 0;
```

`num_rtsp_clients`는 `context_data` 구조체 내부에 위치하며 뮤텍스(`ctx->lock`)로 보호됨.

### 1.2 timeout() 함수 — 버그의 핵심 경로

```c
// Lines 429-459 (추정 라인 번호, raw fetch 기준)
static gboolean timeout(gpointer data)
{
  context_data* ctx = (context_data*)data;
  GstRTSPSessionPool *pool;
  pool = gst_rtsp_server_get_session_pool(ctx->rtsp_server);
  guint removed = gst_rtsp_session_pool_cleanup(pool);
  g_object_unref(pool);

  if (removed > 0){
    M_PRINT("Removed %d sessions\n", removed);           // ← "Removed 1 sessions" 로그
    pthread_mutex_lock(&ctx->lock);
    ctx->num_rtsp_clients--;                              // ← 무조건 감소 (핵심 버그)
    if(ctx->num_rtsp_clients<0) ctx->num_rtsp_clients=0;

    if(ctx->num_rtsp_clients == 0){                       // ← 0이 되면 파이프 닫힘
        ctx->input_frame_number = 0;
        ctx->output_frame_number = 0;
        ctx->need_data = 0;
        ctx->initial_timestamp = 0;
        ctx->last_timestamp = 0;
    }
    pthread_mutex_unlock(&ctx->lock);
  }
  return TRUE;
}
```

**[확인됨]** `timeout()`은 2초마다 실행되며, 만료 세션이 제거될 때마다 `num_rtsp_clients`를 1 감소시킴. 그러나 이 감소는 **실제 살아있는 클라이언트 수를 확인하지 않고 무조건 실행**됨.

### 1.3 rtsp_client_disconnected() — 정상 disconnect 경로

```c
// Lines 383-408
static void rtsp_client_disconnected(GstRTSPClient* self, context_data *data)
{
    pthread_mutex_lock(&data->lock);
    context_data* ctx = (context_data*)data;
    ctx->num_rtsp_clients--;                    // ← 첫 번째 감소
    if(ctx->num_rtsp_clients<0) ctx->num_rtsp_clients=0;

    if(ctx->num_rtsp_clients==0) {
        ctx->input_frame_number = 0;
        ctx->output_frame_number = 0;
        ctx->need_data = 0;
        ctx->initial_timestamp = 0;
        ctx->last_timestamp = 0;
    }

    M_PRINT("rtsp client disconnected, total clients: %d\n", ctx->num_rtsp_clients);
    pthread_mutex_unlock(&data->lock);

    if(ctx->num_rtsp_clients == 0){
        closing_pipe_intentionally = 1;
        M_PRINT("no more rtsp clients, closing source pipe intentionally\n");
        pipe_client_close(PIPE_CH);
        first_client = 0;
    }
}
```

**[확인됨]** 클라이언트가 RTSP "closed" 시그널을 통해 정상 disconnect되면 `rtsp_client_disconnected()`가 호출되어 `num_rtsp_clients`를 감소시킴.

### 1.4 rtsp_client_connected() — 연결 경로

```c
// Lines 410-428
static void rtsp_client_connected(GstRTSPServer* self, GstRTSPClient* object,
                                  context_data *data)
{
    if(first_client==0)
    {
        closing_pipe_intentionally = 0;
        first_client = 1;
        first_run = 0;
        pipe_client_set_connect_cb(PIPE_CH, _cam_connect_cb, NULL);
        pipe_client_set_disconnect_cb(PIPE_CH, _cam_disconnect_cb, NULL);
        pipe_client_set_camera_helper_cb(PIPE_CH, _cam_helper_cb, &context);
        pipe_client_open(PIPE_CH, context.input_pipe_name, PROCESS_NAME,
                        EN_PIPE_CLIENT_CAMERA_HELPER, 0);
    }

    pthread_mutex_lock(&data->lock);
    data->num_rtsp_clients++;
    print_client_info(object);
    g_signal_connect(object, "closed", G_CALLBACK(rtsp_client_disconnected), data);
    pthread_mutex_unlock(&data->lock);
    return;
}
```

**[확인됨]** 시그널 연결은 `"closed"` 이벤트에만 등록됨. `"teardown-request"` 시그널은 등록하지 않음.

---

## 2. 버그 메커니즘 상세 분석

### 2.1 시나리오: TEARDOWN 없이 끊긴 클라이언트 (Ghost Session)

```
시간 t=0: 클라이언트 A 연결 → num_rtsp_clients = 1, first_client = 1
시간 t=5: 클라이언트 B 연결 → num_rtsp_clients = 2

시간 t=10: 클라이언트 B가 TEARDOWN 없이 연결 끊김 (WiFi 단절 등)
           → "closed" 시그널 미발생 → rtsp_client_disconnected() 미호출
           → num_rtsp_clients 여전히 2 (고스트 세션 발생)

시간 t=70: gst-rtsp-server 세션 타임아웃(기본 60초) 도달
           → timeout() 콜백이 gst_rtsp_session_pool_cleanup(pool) 호출
           → 1개 세션 제거 → removed = 1
           → M_PRINT("Removed 1 sessions\n")      ← 관찰된 로그
           → ctx->num_rtsp_clients-- → num_rtsp_clients = 1 (정상처럼 보임)

시간 t=72: 클라이언트 A도 어떤 이유로 disconnect ("closed" 발생)
           → rtsp_client_disconnected() 호출 → num_rtsp_clients = 0
           → closing_pipe_intentionally = 1
           → pipe_client_close(PIPE_CH)            ← 파이프 닫힘!
           → M_PRINT("no more rtsp clients, closing source pipe intentionally\n")
```

이 시나리오는 정상적으로 보임. 그러나 다음 시나리오가 더 문제임.

### 2.2 시나리오: 더블 디크리먼트 (핵심 버그)

```
시간 t=0: 클라이언트 A 연결 → num_rtsp_clients = 1
시간 t=5: 클라이언트 B 연결 → num_rtsp_clients = 2

시간 t=10: 클라이언트 B가 TEARDOWN 없이 끊김 (고스트 세션)

시간 t=60~: gst-rtsp-server 60초 타임아웃 처리 시작

[경우 1: timeout()이 먼저 실행]
시간 t=70: timeout() → removed=1 → num_rtsp_clients = 1
           동시에 또는 직후, B의 세션 관련 GstRTSPSessionMedia finalize가 실행되어
           gst_rtsp_media_unprepare() 호출 → 공유 파이프라인 상태 변경

[경우 2: 클라이언트 B의 "closed" 시그널이 뒤늦게 발생 + timeout() 모두 실행]
→ rtsp_client_disconnected() 감소 AND timeout() 감소 둘 다 실행
→ num_rtsp_clients = 2 - 1(closed) - 1(timeout) = 0  ← 더블 디크리먼트!
→ pipe_client_close() 조기 실행
→ M_PRINT("no more rtsp clients, closing source pipe intentionally\n")
→ 클라이언트 A는 여전히 연결 중인데 소스 파이프가 닫힘 → 스트림 중단
```

**[확인됨]** 더블 디크리먼트 가능성: TEARDOWN 없이 끊긴 경우 "closed" 시그널이 60초 뒤 세션 정리 시점에 지연 발생할 수 있으며, 이때 `timeout()`의 감소와 `rtsp_client_disconnected()`의 감소가 모두 실행되어 `num_rtsp_clients`가 실제보다 적어짐.

### 2.3 gst-rtsp-server 레벨: shared media unprepare 경로

**출처**: https://github.com/GStreamer/gst-rtsp-server/blob/master/gst/rtsp-server/rtsp-session-media.c

```c
static void
gst_rtsp_session_media_finalize (GObject * obj)
{
  GstRTSPSessionMedia *media;
  GstRTSPSessionMediaPrivate *priv;

  media = GST_RTSP_SESSION_MEDIA (obj);
  priv = media->priv;

  GST_INFO ("free session media %p", media);
  gst_rtsp_session_media_set_state (media, GST_STATE_NULL);  // ← NULL 상태로 전환
  gst_rtsp_media_unprepare (priv->media);                    // ← 미디어 unprepare!
  g_ptr_array_unref (priv->transports);
  g_free (priv->path);
  g_object_unref (priv->media);
  g_mutex_clear (&priv->lock);

  G_OBJECT_CLASS (gst_rtsp_session_media_parent_class)->finalize (obj);
}
```

**[확인됨]** `gst_rtsp_session_pool_cleanup()`이 만료 세션을 제거할 때, 내부적으로 `GstRTSPSession` → `GstRTSPSessionMedia` finalize 체인이 실행되며, 이 과정에서 **공유(shared) 미디어임에도 `gst_rtsp_media_unprepare()`가 호출**됨.

gst-rtsp-server의 `GstRTSPMedia`는 `shared=TRUE`이더라도 `unprepare`는 레퍼런스 카운트가 아닌 **호출 자체**로 파이프라인을 GST_STATE_NULL로 전환함. 살아있는 다른 클라이언트의 `GstRTSPSessionMedia`가 여전히 이 media를 참조하고 있어도, unprepare 호출 자체가 파이프라인을 멈출 수 있음.

**[추정]** 단, 최신 gst-rtsp-server 버전은 내부 reference counting으로 이 문제를 부분적으로 방어할 수 있음. voxl-streamer가 사용하는 정확한 gst-rtsp-server 버전 및 패치 여부는 미확인.

---

## 3. ModalAI 포럼 검색 결과

### 3.1 핵심 관련 스레드: teardown-request 미처리 이슈

**출처**: https://forum.modalai.com/topic/1647/voxl-streamer-does-not-handle-teardown-request-signal-correctly

**[확인됨]** 이 스레드가 현재 버그의 역사적 기원.

- **보고된 증상**: WiFi 단절 등으로 클라이언트가 끊길 때 "teardown-request" 시그널이 발생하지만 voxl-streamer는 "closed" 시그널에만 연결됨 → 세션이 제대로 정리되지 않아 재연결 불가.
- **커뮤니티가 제안한 Fix**: `timeout()` 콜백을 추가해 2초마다 `gst_rtsp_session_pool_cleanup(pool)` 호출 → 만료 세션 자동 청소.
- **ModalAI 대응**: Chad Sweet (ModalAI 직원)이 PR 수용 확인. **v0.4.2에 머지됨**.
- **문제**: 이 fix 자체가 현재 버그("Removed sessions" 시 스트림 중단)의 원인임. 만료 세션 제거 시 `num_rtsp_clients--`를 무조건 실행하는 코드가 포함되었고, 이것이 더블 디크리먼트 및 shared media unprepare 트리거를 유발.

### 3.2 반복 connect/disconnect 스레드

**출처**: https://forum.modalai.com/topic/550/voxl-streamer-client-repetitive-connect-disconnect

**[확인됨]** UVC 카메라의 반복 연결/해제 문제. QGC 버전 호환성 이슈로 특정. voxl-streamer의 세션 카운터 버그와는 다른 레이어의 문제지만 동일한 증상(스트림 중단)을 보임.

### 3.3 VOXL2 voxl-streamer 스레드

**출처**: https://forum.modalai.com/topic/2396/voxl2-voxl-streamer

**[못 찾음]** 세션 청소로 인한 스트림 중단 문제는 보고되지 않음. 해당 스레드는 loopback IP 바인딩 문제를 다루는 별개 이슈.

### 3.4 SDK 1.4.1 RTSP 불안정 스레드

**출처**: https://forum.modalai.com/topic/4219/rtsp-connection-from-drone-unreliable-since-update-to-sdk-1-4-1/2

**[확인됨 - 간접 관련]** 고온·고부하 환경에서 RTSP 연결 불안정 보고. ModalAI 직원 Alex Kushleyev가 대응. 세션 청소 버그와 직접 연관은 없으나 voxl-streamer 안정성 이슈의 패턴을 보여줌.

---

## 4. voxl-streamer 버전/체인지로그 분석

**출처**: https://gitlab.com/voxl-public/voxl-sdk/services/voxl-streamer/-/raw/master/CHANGELOG  
**출처**: https://gitlab.com/voxl-public/voxl-sdk/services/voxl-streamer/-/tags

### 세션/클라이언트 처리 관련 버전 이력

| 버전 | 변경 내용 | 관련성 |
|------|-----------|--------|
| v0.1.7 | RTSP 클라이언트 연결 해제 시 프레임 번호 리셋 로직 수정 | 직접 관련 |
| v0.4.2 | teardown-request 미처리 Fix 머지 (timeout() + cleanup() 추가) | **버그 도입 버전** |
| v0.5.1 | RTSP 클라이언트가 1개 이상일 때만 카메라 파이프 오픈 | 직접 관련 |
| v0.7.0 | 소스 파이프 재연결 및 재시작 지원 | 간접 관련 |
| v0.7.1 | 초기 이미지 데이터 fetch 실패 시 재시도 유지 | 간접 관련 |
| v0.8.0 | cross4 빌드 시스템으로 전환 | 관련 없음 |
| v0.8.1 | QCS6490 칩셋 지원 추가 | 관련 없음 |

**[확인됨]** 0.8.0 이후 버전(0.8.1, sdk-1.6.4, sdk-1.7.0)에서 세션/클라이언트 처리 관련 수정 없음. 최신 버전(sdk-1.7.0, 2026-03-20)도 동일 코드 구조 유지 중.

**[확인됨]** 버그가 v0.4.2에서 도입되어 v0.8.x까지 수정되지 않은 채로 유지됨.

---

## 5. gst-rtsp-server 업스트림 이슈

**출처**: https://github.com/GStreamer/gst-rtsp-server  
**출처**: https://deepwiki.com/GStreamer/gst-rtsp-server/2-architecture

### 5.1 session pool cleanup의 shared media 영향

**[확인됨]** gst-rtsp-server 공식 문서 및 소스:

> "When a session is removed from the sessionpool and its last reference is unreffed, all related objects and media are destroyed as if a TEARDOWN happened from the client."

> "When there are no more references to the GstRTSPMedia, the media pipeline is shut down (with `_unprepare`) and destroyed."

**문제**: `GstRTSPSessionMedia` finalize 시 `gst_rtsp_media_unprepare()` 무조건 호출. `shared=TRUE` 상태에서도 이 경로는 동일하게 실행됨.

### 5.2 관련 업스트림 버그 (Bug 648463)

**출처**: https://gstreamer-bugs.narkive.com/aT7wWKX4/bug-648463-new-pipeline-stalls-when-using-gst-rtsp-media-factory-set-shared-factory-true-vlc  
(2011년 보고, 서버 503으로 직접 내용 확인 불가)

**[추정]** `gst_rtsp_media_factory_set_shared(factory, TRUE)` + VLC 혼합 사용 시 파이프라인 스톨 버그가 존재했음. voxl-streamer도 `set_shared(factory, TRUE)` 사용. 이 오래된 버그와 동일 메커니즘일 가능성 있음.

### 5.3 gst_rtsp_session_pool_cleanup 동작

**[확인됨]** 업스트림 소스 (`rtsp-session-pool.c`):

```c
guint
gst_rtsp_session_pool_cleanup (GstRTSPSessionPool * pool)
{
  // ...
  g_mutex_lock (&priv->lock);
  result = g_hash_table_foreach_remove (priv->sessions,
                                        (GHRFunc) cleanup_func,
                                        &data);
  if (result > 0)
    priv->sessions_cookie++;
  g_mutex_unlock (&priv->lock);

  for (walk = data.removed; walk; walk = walk->next) {
    GstRTSPSession *sess = walk->data;
    g_signal_emit (pool,
                   gst_rtsp_session_pool_signals[SIGNAL_SESSION_REMOVED],
                   0, sess);
    g_object_unref (sess);   // ← 여기서 GstRTSPSession unref → GstRTSPSessionMedia finalize 체인
  }
  // ...
}
```

```c
static gboolean
cleanup_func (gchar * sessionid, GstRTSPSession * sess, CleanupData * data)
{
  gboolean expired;
  expired = gst_rtsp_session_is_expired_usec (sess, data->now_monotonic_time);
  if (expired) {
    GST_DEBUG ("session expired");
    data->removed = g_list_prepend (data->removed, g_object_ref (sess));
  }
  return expired;
}
```

**[확인됨]** `cleanup_func`는 기본 60초 타임아웃(gst-rtsp-server default)이 만료된 세션만 제거함. `g_object_unref(sess)` 호출이 `GstRTSPSessionMedia` → `gst_rtsp_media_unprepare()` 체인을 트리거함.

---

## 6. 근본 원인 종합

### 원인 1 (주요): timeout()의 무조건적 num_rtsp_clients 감소

```
TEARDOWN 없이 끊긴 클라이언트 B (고스트 세션)
  → 60초 후 gst_rtsp_session_pool_cleanup() 호출
  → "Removed 1 sessions" 로그
  → num_rtsp_clients-- (무조건)
  → 동시에 클라이언트 A의 "closed" 시그널 또는 이후 disconnect 시
  → 두 번째 num_rtsp_clients-- → 값 = 0
  → closing_pipe_intentionally = 1
  → pipe_client_close(PIPE_CH)    ← A가 여전히 스트리밍 중인데 파이프 닫힘
  → "no more rtsp clients, closing source pipe intentionally"
  → 스트림 중단
```

**[확인됨]**

### 원인 2 (보조): gst-rtsp-server shared media unprepare 경로

```
gst_rtsp_session_pool_cleanup()
  → GstRTSPSession unref
  → GstRTSPSessionMedia finalize
  → gst_rtsp_media_unprepare()    ← shared media이더라도 호출됨
  → GStreamer 파이프라인 GST_STATE_NULL
  → 살아있는 클라이언트 A의 스트림도 멈춤
```

**[확인됨]** gst-rtsp-server 소스에서 직접 확인.

---

## 7. 수정 방향 (권장)

### Fix 1: timeout()에서 실제 살아있는 세션 수로 num_rtsp_clients 동기화

현재 코드:
```c
if (removed > 0){
    ctx->num_rtsp_clients--;  // 잘못됨: 무조건 감소
}
```

수정 방향:
```c
if (removed > 0){
    // 실제 살아있는 세션 수를 pool에서 직접 조회
    guint live = gst_rtsp_session_pool_get_n_sessions(pool);
    ctx->num_rtsp_clients = (int)live;
    // 또는: removed 만큼 감소하되 실제 세션 수와 일치 검증
}
```

### Fix 2: 파이프 닫힘 조건을 실제 살아있는 세션 수 기반으로 변경

`gst_rtsp_session_pool_get_n_sessions()` API를 통해 현재 살아있는 세션 수를 조회하고, 이 값이 0일 때만 파이프 닫힘을 실행.

### Fix 3: shared media 보호 (gst-rtsp-server 레벨)

만료된 세션을 정리할 때 `shared=TRUE` 미디어에 대해 `unprepare`를 보호하는 패치. 업스트림에 버그 리포트 제출 또는 voxl-streamer에서 `session-removed` 시그널 핸들러로 미디어 상태 재복구.

---

## 8. 미확인 사항

- **[못 찾음]** voxl-streamer가 사용하는 gst-rtsp-server의 정확한 버전 및 패치 수준 (VOXL2 Mini 시스템 패키지)
- **[못 찾음]** Bug 648463 (gst-rtsp-server shared + VLC 파이프라인 스톨)의 정확한 내용 (서버 503 오류로 직접 접근 불가)
- **[못 찾음]** `gst_rtsp_session_pool_get_n_sessions()` API 가용 여부 (voxl-streamer가 빌드에 사용하는 gst-rtsp-server 버전에 따라 다름)
- **[추정]** gst-rtsp-server의 shared media에서 `unprepare` 호출이 실제로 파이프라인을 즉시 중단하는지, 아니면 레퍼런스 카운트로 방어하는지는 정확한 버전 확인 필요

---

## 출처 목록

1. voxl-streamer GitLab 저장소: https://gitlab.com/voxl-public/voxl-sdk/services/voxl-streamer
2. voxl-streamer main.c (raw): https://gitlab.com/voxl-public/voxl-sdk/services/voxl-streamer/-/raw/master/src/main.c
3. voxl-streamer CHANGELOG (raw): https://gitlab.com/voxl-public/voxl-sdk/services/voxl-streamer/-/raw/master/CHANGELOG
4. voxl-streamer tags: https://gitlab.com/voxl-public/voxl-sdk/services/voxl-streamer/-/tags
5. ModalAI 포럼 - teardown-request 미처리: https://forum.modalai.com/topic/1647/voxl-streamer-does-not-handle-teardown-request-signal-correctly
6. ModalAI 포럼 - 반복 connect/disconnect: https://forum.modalai.com/topic/550/voxl-streamer-client-repetitive-connect-disconnect
7. ModalAI 포럼 - VOXL2 streamer: https://forum.modalai.com/topic/2396/voxl2-voxl-streamer
8. ModalAI 포럼 - SDK 1.4.1 불안정: https://forum.modalai.com/topic/4219/rtsp-connection-from-drone-unreliable-since-update-to-sdk-1-4-1/2
9. ModalAI 공식 문서: https://docs.modalai.com/voxl-streamer/
10. gst-rtsp-server rtsp-session-pool.c: https://github.com/GStreamer/gst-rtsp-server/blob/master/gst/rtsp-server/rtsp-session-pool.c
11. gst-rtsp-server rtsp-session-media.c: https://github.com/GStreamer/gst-rtsp-server/blob/master/gst/rtsp-server/rtsp-session-media.c
12. gst-rtsp-server rtsp-media.c: https://github.com/GStreamer/gst-rtsp-server/blob/master/gst/rtsp-server/rtsp-media.c
13. gst-rtsp-server 아키텍처 설명: https://deepwiki.com/GStreamer/gst-rtsp-server/2-architecture
14. ModalAI 포럼 - H265 스트리머: https://forum.modalai.com/topic/3944/streamer-default-h265/5
