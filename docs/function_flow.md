# 함수 흐름 — Phase 3

> 최종 `server`는 `src/app/main_ev.c` + `src/app/server_ev.c` 기반의 **이벤트 루프 + task pool** 서버다.
> Phase 3 프록시는 `src/proxy/proxy.c`가 담당하고, `proxy_pool.c`가 backend 연결 pool을, `proxy_wire.c`가 요청/응답 헤더 재작성을, `http_upstream.c`/`chunked.c`가 파서를 보조한다.
> 이 문서는 현재 코드 기준 실행 흐름을 설명한다. Phase 1 함수 레퍼런스는 [function_reference.md](function_reference.md)에 남아 있다.

이 문서는 “어떤 함수가 어떤 순서로 호출되는가”를 따라간다. 함수별 세부 구현은 각 소스와 [phase3-design.md](phase3-design.md)를 본다.

---

## 1. 전체 호출 흐름

### 시작

```text
main                         src/app/main_ev.c
├─ config_init
├─ config_parse_cli
│  ├─ config_parse_file      (--config가 있을 때)
│  ├─ parse_int_arg
│  └─ parse_size_arg
├─ config_validate
└─ server_ev_run             src/app/server_ev.c
   ├─ setup_signals
   ├─ create_listen_socket
   ├─ ev_create_backend
   ├─ ev_add(listenfd)
   ├─ task_pool_create
   ├─ ev_add(task completion fd)
   ├─ ev_add(signal pipe)
   ├─ logger_start
   ├─ proxy_init
   └─ event loop
```

### 요청 하나

```text
ev_wait
└─ accept_new
   └─ conn_new
      └─ ev_add(client fd, EV_READ)

EV_READ
└─ on_readable
   ├─ 헤더 읽기 루프
   └─ process_buffer
      ├─ proxy route  → proxy_start
      ├─ /cgi-bin/    → submit_cgi → task pool
      └─ 그 외        → static_file_prepare → conn_build_static

EV_WRITE
└─ on_writable
   ├─ c->out 전송
   ├─ 파일 body 전송
   └─ keep-alive면 ST_READ, 아니면 close
```

프록시 요청은 client fd가 `ST_PROXY`가 되고, 이후 client 이벤트는 `proxy_handle_client_event()`가, upstream 이벤트는 `proxy_handle_upstream_event()`가 처리한다.

---

## 2. 시작 흐름

### `main()`

`src/app/main_ev.c`

1. `config_init()`으로 기본값을 채운다.
2. `config_parse_cli()`가 `--config` 파일을 먼저 읽고, 그다음 positional 인자와 CLI 옵션으로 덮어쓴다.
3. `config_validate()`가 포트, timeout, route, docroot, CGI 디렉터리, `max_conns`, `max_upstreams`를 검증한다.
4. `server_ev_run()`을 호출한다.

설정 우선순위는 **CLI > 설정 파일 > 기본값**이다.

### `config_parse_cli()`

`src/common/config.c`

- `--config <file>`이 있으면 `config_parse_file()`을 먼저 호출한다.
- 이후 argv를 다시 훑으면서 `--timeout`, `--max-uri`, `--max-header`, `--max-body`, `--keepalive-idle`, `--keepalive-max`, `--task-workers`, `--task-queue`, `--max-conns`, `--max-upstreams`를 적용한다.
- `positional` 인자는 `<port> <docroot> [cgi_dir]` 순서다.
- 설정 파일을 읽은 경우 positional 인자는 필수가 아니다.

`config_parse_file()`은 `key value` 형식과 `[route <prefix>]`, `[global]` 섹션을 읽는다. 줄 번호를 붙여 fail-fast 한다.

### `server_ev_run()`

`src/app/server_ev.c`

호출 순서:

1. `setup_signals()`
   - `SIGINT`/`SIGTERM` 핸들러 등록
   - self-pipe 생성
2. `create_listen_socket(cfg->port)`
3. `ev_create_backend(getenv("EV_BACKEND"))`
   - 기본값은 플랫폼별 epoll/kqueue/poll
4. listen fd를 `EV_READ`로 등록
5. `task_pool_create(cfg->task_workers, cfg->task_queue_size)`
6. task pool completion fd와 signal pipe를 이벤트 루프에 등록
7. `logger_start()`
8. `proxy_init(cfg)`
   - route/backend 복사
   - backend hostname을 시작 시 1회 resolve
   - pool/health 카운터 초기화
9. 이벤트 루프 진입

`server`와 `server_ev`는 같은 `EV_SERVER_OBJS`를 링크한다. 즉 둘은 동일한 실행 코드다.

---

## 3. 이벤트 루프

`server_ev_run()`의 메인 루프는 대략 다음과 같다.

```c
while (!g_stop || !draining || head != NULL) {
    if (g_stop && !draining) {
        draining = 1;
        begin_drain(...);
    }

    int timeout = next_timeout(head);
    int n = ev_wait(loop, events, 64, timeout);

    for (int i = 0; i < n; i++) {
        if (fd == listenfd)             accept_new();
        else if (fd == completion_fd)   drain_task_completions();
        else if (fd == signal_pipe)     drain_signal_pipe();
        else if (src->kind == EV_SRC_UPSTREAM)
            proxy_handle_upstream_event(src, events);
        else if (c->state == ST_PROXY)
            proxy_handle_client_event(c, events);
        else {
            on_readable(...);           // EV_READ
            on_writable(...);           // EV_WRITE
        }
    }

    conn_sweep(&head, &count);
    expire_timers(loop, head, cfg);
    proxy_sweep();
    conn_sweep(&head, &count);
    proxy_reap();
}
```

### 이벤트 source 구분

`ev_event_t.ptr`은 `ev_source_t *`다.

- `EV_SRC_CLIENT`: `conn_t`
- `EV_SRC_UPSTREAM`: `proxy_conn_t`

`conn_t`와 `proxy_conn_t` 모두 `ev_source_t source`를 **첫 필드**로 가진다. 이벤트 루프는 `source.kind`를 보고 어느 쪽인지 구분한다. 이 구분이 틀리면 UAF/close 오류가 나기 쉬우므로 `include/ev_source.h`의 규칙을 유지해야 한다.

### 타임아웃 소유권

- 일반 client 연결: `expire_timers()`가 `c->deadline_ms`를 본다.
- 프록시 연결: `expire_timers()`는 `ST_PROXY`를 건너뛰고 `proxy_sweep()`이 처리한다.
- `next_timeout()`은 일반 연결 deadline과 `proxy_next_timeout_ms()` 중 작은 값을 `ev_wait()` timeout으로 넘긴다.

프록시 timeout을 `expire_timers()`와 `proxy_sweep()` 양쪽에서 처리하지 않도록 주의한다.

### 루프 끝 정리

매 이벤트 배치가 끝난 뒤:

1. `conn_sweep()` — 닫힌 `conn_t` 제거
2. `expire_timers()` — 일반 연결의 request/keep-alive/CGI deadline
3. `proxy_sweep()` — 프록시 active deadline, backend/pool idle timeout
4. `conn_sweep()` — 프록시가 닫은 client 정리
5. `proxy_reap()` — `dead` 표시된 `proxy_conn_t` 실제 free

`proxy_reap()`가 따로 있는 이유는 이벤트 콜백 도중 `free()`하면 같은 이벤트 배치의 다른 이벤트가 같은 포인터를 만질 수 있기 때문이다.

---

## 4. 연결 수락과 상태

### `accept_new()`

`src/app/server_ev.c`

- `accept()`를 non-blocking으로 반복한다.
- `*count >= cfg->max_conns`이면 `reject_overloaded()`로 `503 + Retry-After: 1`을 보내고 닫는다.
- `set_nonblocking()`, `set_cloexec()` 후 `conn_new()`.
- `ev_add(fd, EV_READ, c)`로 등록하고 연결 리스트에 넣는다.

### `conn_t` 상태

`include/server_conn.h`

| 상태 | 의미 |
| --- | --- |
| `ST_READ` | 다음 요청 헤더를 읽는 중 |
| `ST_READ_BODY` | CGI용 요청 body를 다 읽는 중 |
| `ST_WAIT_TASK` | CGI task pool 결과를 기다리는 중 |
| `ST_PROXY` | 프록시가 client/upstream을 중계하는 중 |
| `ST_WRITE` | 응답 헤더/body를 쓰는 중 |

`ST_WAIT_TASK`에서는 client fd를 이벤트 루프에서 제거한다. task가 끝나면 completion fd를 통해 다시 `EV_WRITE`로 붙인다.

---

## 5. 요청 처리 — `on_readable()` → `process_buffer()`

### 헤더 읽기

`on_readable()`은 `ST_READ`에서 `\r\n\r\n`이 나올 때까지 `c->in`에 읽는다.

- buffer가 가득 차면 `has_crlf()`와 `request_line_uri_too_long()`으로 원인을 구분해 `414` 또는 `431`을 만든다.
- keep-alive 연결에서 `in_len == 0`으로 idle이 시작되면 request timeout deadline을 갱신한다.

### `process_buffer()`

`src/app/server_ev.c`

1. `http_request_init()`
2. `http_request_parse_buffer()`
   - `HTTP_PARSE_INCOMPLETE`는 `400`으로 처리
   - 파싱 실패는 `conn_build_error()`
3. `config_find_route()`로 longest-prefix route 선택
4. 분기:
   - proxy route → `proxy_start()`
   - `/cgi-bin/` → `submit_cgi()`
   - 그 외 → `static_file_prepare()` → `conn_build_static()`

프록시 분기가 chunked 거부보다 먼저다. 그래서 **chunked 요청은 프록시 route에서만** stream-through되고, 정적/CGI route에서는 `501`이다.

### proxy 분기

`proxy_start()`가 `1`을 반환하면:

- 이미 `c->in`에 들어온 body 일부를 `c->in` 앞으로 옮긴다.
- body 뒤에 pipelined byte가 더 있으면 `proxy_mark_close()`로 표시한다.
- `proxy_kick()`으로 첫 read/write를 시작한다.

`proxy_start()`가 `0`을 반환하면 `502`/`503` 같은 에러 응답을 이미 만든 것이므로 `c->in_len`을 비운다.

### 정적 분기

`static_file_prepare()`가 `static_result_t`를 만들고, `conn_build_static()`이 응답 헤더와 파일 전송 상태를 `conn_t`에 넣는다. 실제 파일 전송은 `on_writable()`에서 한다.

### CGI 분기

- body가 아직 다 오지 않았으면 `ST_READ_BODY`로 바꾸고 body buffer를 할당한다.
- `Expect: 100-continue`가 있으면 여기서 `100 Continue`를 직접 보낸다.
- body가 준비되면 `submit_cgi()`.

### keep-alive 복귀

`on_writable()`이 응답을 모두 보내면:

- `close_after`이면 연결을 닫는다.
- 아니면 `ST_READ`로 돌아가고 `keepalive_idle` 또는 request timeout을 설정한다.
- `c->in`에 다음 요청이 이미 들어와 있으면 바로 `process_buffer()`를 호출한다.

---

## 6. 정적 파일 흐름

```text
process_buffer
└─ static_file_prepare
   ├─ docroot realpath 검증
   ├─ path traversal / symlink escape 차단
   ├─ Range / If-Modified-Since
   └─ static_result_t

conn_build_static
├─ 응답 헤더 생성
├─ file_fd / file_remaining / file_offset 설정
└─ c->state = ST_WRITE

on_writable
├─ c->out 전송
├─ file_fd에서 chunk read → client write
└─ 완료 후 file_fd close, keep-alive 또는 close
```

파일 전체를 메모리에 올리지 않고 `EV_FILE_CHUNK` 단위로 읽어서 보낸다.

---

## 7. CGI 흐름

```text
process_buffer
└─ submit_cgi
   ├─ cgi_job_create
   ├─ task_pool_submit
   ├─ c->state = ST_WAIT_TASK
   └─ ev_del(client fd)

worker thread
└─ cgi_job_execute
   ├─ posix_spawn
   ├─ stdin/stdout pipe
   ├─ timeout / signal 처리
   └─ cgi_result_t

completion fd
└─ drain_task_completions
   ├─ find_by_task
   ├─ 응답을 c->out에 연결
   ├─ c->state = ST_WRITE
   └─ ev_add(client fd, EV_WRITE)

on_writable
└─ 응답 전송 후 keep-alive 또는 close
```

CGI worker는 event loop와 별도 스레드에서 실행된다. task queue가 가득 차면 `task_pool_submit()`이 `EAGAIN`을 반환하고, 서버는 `503 + Retry-After`를 만든다.

---

## 8. 프록시 흐름

### 8.1 초기화 — `proxy_init()`

`src/proxy/proxy.c`

1. 설정의 route/backend를 내부 `proxy_route_t`/`proxy_backend_t`로 복사한다.
2. backend hostname을 `getaddrinfo()`로 **시작 시 1회** resolve한다.
3. timeout, `max_upstreams`, pool 크기, health cooldown을 전역 설정에 옮긴다.
4. 실패하면 `proxy_cleanup()` 후 시작 실패.

### 8.2 시작 — `proxy_start()`

```text
proxy_start
├─ route/backend 유효성 확인
├─ max_upstreams 확인 (초과 시 503)
├─ proxy_conn_t calloc
├─ body mode 결정
│  ├─ BODY_CHUNKED
│  ├─ BODY_CONTENT_LENGTH
│  └─ BODY_NONE
├─ proxy_build_request
├─ c->proxy = p, c->state = ST_PROXY
├─ proxy_list_add, g_active_upstreams++
└─ proxy_connect_backend
```

`proxy_build_request()`는 (`src/proxy/proxy_wire.c`):

- `strip_prefix` 적용 여부에 따라 path를 다시 만든다.
- query를 유지한다.
- `Host`, `X-Forwarded-For`, `X-Forwarded-Proto`, `X-Forwarded-Host`를 넣는다.
- hop-by-hop 헤더와 `Connection`에 이름이 적힌 헤더를 제거한다.
- `Content-Length` 또는 `Transfer-Encoding: chunked`를 다시 붙인다.
- `Expect`는 프록시가 직접 처리하므로 upstream에는 보내지 않는다.

### 8.3 backend 연결 — `proxy_connect_backend()`

```text
select_backend(route, tried_mask)
├─ round-robin cursor
├─ down_until_ms가 지난 backend만 후보
└─ 이미 시도한 backend는 skip

proxy_pool_acquire(backend)
├─ idle fd가 있으면 MSG_PEEK로 stale 검사
└─ 정상이면 재사용

없으면 socket → non-blocking connect
├─ 즉시 성공: PROXY_SENDING
└─ EINPROGRESS: PROXY_CONNECTING
```

connect 실패 시 backend를 `down_until_ms`로 표시하고 다음 backend를 시도한다. 모든 backend가 실패하면 `502`다.

### 8.4 이벤트 등록 — `proxy_update_events()`

현재 상태에 따라 client와 upstream의 event interest를 다시 계산한다.

- `PROXY_SENDING`: 요청 헤더/body를 더 보낼 수 있으면 upstream `EV_WRITE`, client body를 읽을 수 있으면 client `EV_READ`
- `PROXY_READING_HEADERS`: upstream `EV_READ`
- `PROXY_RELAY_BODY`: upstream `EV_READ` + client `EV_WRITE`
- client 응답/100-continue가 남아 있으면 client `EV_WRITE`

요청 헤더와 body가 모두 전송되면 `PROXY_SENDING → PROXY_READING_HEADERS`로 전환하고 응답 deadline을 건다.

### 8.5 upstream 이벤트 — `proxy_handle_upstream_event()`

```text
EV_WRITE
├─ CONNECTING: getsockopt(SO_ERROR)
│  ├─ 실패: backend down, 다음 backend
│  └─ 성공: SENDING
└─ SENDING/RELAY_BODY: proxy_flush_upstream

EV_READ / EV_ERR
├─ READING_HEADERS: proxy_read_upstream_headers
└─ RELAY_BODY: proxy_read_upstream_body
   └─ EOF면 proxy_drain_eof

EV_ERR
├─ 읽을 데이터가 남아 있으면 먼저 읽는다
└─ 응답이 시작되지 않았으면 502, 시작됐으면 close
```

`EV_ERR`를 곧바로 `abort`하지 않고 **읽을 수 있는 데이터를 먼저 읽는 것**이 중요하다. `Connection: close`, close-delimited body, chunked+close가 여기서 처리된다.

### 8.6 client 이벤트 — `proxy_handle_client_event()`

client fd가 `ST_PROXY`일 때의 이벤트다.

```text
EV_ERR
└─ proxy_abort

EV_READ
├─ PROXY_SENDING && !req_body_done → proxy_read_client_body
└─ 그 외 → proxy_abort

EV_WRITE
├─ 100-continue가 남아 있으면 proxy_flush_continue
└─ 그다음 proxy_flush_client
```

`proxy_start()` 직후에는 `proxy_kick()`이 `c->in`에 이미 들어온 body를 먼저 처리한다. 이후 client body는 `EV_READ`, 응답 전송은 `EV_WRITE`로 진행한다.

### 8.7 요청 body — `proxy_read_client_body()`

- `BODY_CONTENT_LENGTH`: `c->in`에 있는 body를 우선 소비하고, 부족하면 client fd에서 읽는다.
- `BODY_CHUNKED`: `chunk_parser_feed()`로 framing을 추적하면서 **원문 chunk byte를 그대로** upstream에 전달한다.
- body 크기가 `max_body`를 넘으면 `413`.
- `Expect: 100-continue`는 `proxy_flush_continue()`가 client에 먼저 보낸다.

### 8.8 응답 헤더 — `proxy_read_upstream_headers()`

`http_upstream.c`의 `upstream_response_parse()`를 사용한다.

- status line, reason phrase, 헤더 파싱
- `Content-Length`/`Transfer-Encoding` 충돌 거부
- `1xx`는 건너뛰고 최종 응답을 기다림
- body 종류 결정:
  - `BODY_CONTENT_LENGTH`
  - `BODY_CHUNKED`
  - `BODY_CLOSE`
  - `BODY_NONE` (`HEAD`, `204`, `304`, `205`)
- `proxy_build_response()`로 client용 헤더를 만든다. (`src/proxy/proxy_wire.c`)
- hop-by-hop 헤더와 `Connection` named header를 제거한다.
- chunked 응답의 `Trailer`는 전달한다.

### 8.9 body 중계 — `proxy_read_upstream_body()` / `proxy_flush_client()`

- `BODY_CONTENT_LENGTH`: 남은 길이만큼 읽는다.
- `BODY_CHUNKED`: `chunk_parser_feed()`로 decoded byte 수를 세고, 원문 chunk byte를 그대로 client에 보낸다.
- `BODY_CLOSE`: EOF까지 읽는다.
- `proxy_flush_client()`가 `resp_out`과 `u2c`를 client에 쓰고, body가 끝나면 `proxy_finish()`.

### 8.10 종료 — `proxy_finish()` / `proxy_fail()` / `proxy_abort()`

`proxy_finish()`:

- access log 기록
- upstream이 keep-alive 가능하면 `proxy_pool_release()`
- 아니면 `proxy_close_upstream()`
- `proxy_free_struct()`
- client keep-alive면 `ST_READ`, 아니면 close

`proxy_fail(status)`:

- 응답이 아직 시작되지 않았으면 `conn_build_error(status)`
- 이미 시작됐으면 두 번째 응답을 쓰지 않고 연결 close
- upstream close, `proxy_free_struct()`

`proxy_abort()`:

- client가 끊겼거나 내부 오류일 때 upstream과 client를 정리
- 응답을 새로 만들지 않는다

### 8.11 지연 free

`proxy_free_struct()`는 즉시 `free()`하지 않고:

1. `p->dead = 1`
2. `g_dead` 리스트에 추가
3. `c->proxy = NULL`

이벤트 배치가 끝난 뒤 `proxy_reap()`가 실제 free를 한다. 같은 이벤트 배치에서 client/upstream 이벤트가 겹쳐도 포인터를 안전하게 만드는 장치다.

### 8.12 timeout과 sweep

`proxy_sweep()`:

- pool idle timeout이 지난 fd close
- active proxy deadline 초과:
  - `PROXY_SENDING && !req_body_done` → `408`
  - 그 외 → `504`
- `PROXY_CONNECTING` timeout이면 backend를 down 처리

`proxy_next_timeout_ms()`는 active proxy deadline과 pool idle 만료 중 가장 이른 값을 반환해 `ev_wait()`가 제때 깨어나게 한다.

### 8.13 pool

`proxy_pool_acquire()`:

- `MSG_PEEK`로 idle fd에 예상치 못한 데이터/EOF가 있는지 확인
- stale이면 close하고 다음 idle fd
- 정상이면 fd 반환

`proxy_pool_release()`:

- `pool max_idle`을 넘으면 close
- 아니면 `idle_since_ms`를 기록하고 backend별 리스트에 넣는다

HTTP/1.0 응답이거나 `Connection: close`이면 pool에 넣지 않는다.

---

## 9. 종료 흐름

### 신호

`on_signal()`:

- 첫 `SIGINT`/`SIGTERM`: `g_stop = 1`, self-pipe에 byte write
- 두 번째 신호: `_exit(1)`

`server_ev_run()`은 `g_stop`을 보면:

1. `begin_drain()`
   - listen fd close
   - `ST_READ`/`ST_READ_BODY` 연결은 즉시 close
   - 나머지는 `close_after = 1`
2. 최대 `EV_DRAIN_TIMEOUT_MS`(5초) 동안 진행 중 연결을 기다린다.
3. watchdog thread가 drain 시간을 넘기면 프로세스를 종료한다.

### 정리 순서

```text
proxy_shutdown_all()
→ 남은 conn_close()
→ task_pool_shutdown()
→ task_pool_destroy()
→ listen fd close
→ signal pipe close
→ ev_destroy()
→ proxy_cleanup()
→ logger_stop()
```

`proxy_cleanup()`은 route별 pool fd를 닫고 `proxy_reap()`로 남은 dead proxy를 정리한다.

---

## 10. 모듈 지도

| 파일 | 역할 |
| --- | --- |
| `src/app/main_ev.c` | 진입점, 설정 파싱, `server_ev_run()` 호출 |
| `src/common/config.c` | 기본값, CLI 파싱, 검증, route 매칭 |
| `src/common/config_file.c` | INI 설정 파일 파서 |
| `src/app/server_ev.c` | 이벤트 루프, `conn_t` 상태 머신, 정적/CGI 연결, 종료 |
| `src/event/event_backend.c` | epoll/kqueue/poll backend 선택 |
| `src/event/event_epoll.c` | Linux epoll backend |
| `src/event/event_kqueue.c` | macOS/BSD kqueue backend |
| `src/event/event_poll.c` | fallback poll backend |
| `src/http/http_request.c` | HTTP 요청 파서, 헤더/URI/body 제한 |
| `src/http/http_response.c` | 응답 헤더/에러 응답 생성 |
| `src/static/static_file.c` | docroot 검증, Range, IMS, 정적 파일 준비 |
| `src/cgi/cgi.c` | `posix_spawn`, pipe, CGI 출력 처리 |
| `src/cgi/cgi_task.c` | CGI job 실행과 결과 변환 |
| `src/cgi/task_pool.c` | worker thread + queue + completion fd |
| `src/proxy/proxy.c` | route/backend 선택, 프록시 상태 머신 |
| `src/proxy/proxy_conn.h` | 내부 `proxy_conn_t`/route/state/body 타입 |
| `src/proxy/proxy_wire.c` | 요청/응답 헤더 직렬화 |
| `src/proxy/proxy_pool.c` | backend keep-alive pool |
| `src/http/http_upstream.c` | upstream 응답 헤더 파서 |
| `src/http/chunked.c` | chunked parser (framing, trailer, decoded byte 수) |
| `src/common/access_log.c` | 요청별 access log 상태 |
| `src/common/logger.c` | 로그 큐와 logger thread |

---

## 11. 읽을 때 주의할 점

1. **`ev_source_t`가 항상 첫 필드**
   - `conn_t`와 `proxy_conn_t` 모두 첫 필드가 `source`다.
   - 이벤트 dispatch에서 `EV_SRC_CLIENT`/`EV_SRC_UPSTREAM`을 잘못 구분하면 안 된다.

2. **프록시는 즉시 free하지 않는다**
   - `proxy_free_struct()`는 `dead` 표시만 하고 `proxy_reap()`가 나중에 free한다.
   - 이벤트 콜백 안에서 직접 `free(p)`를 호출하면 안 된다.

3. **프록시 deadline은 `proxy_sweep()` 소유**
   - `expire_timers()`는 `ST_PROXY`를 건너뛴다.
   - `next_timeout()`은 `proxy_next_timeout_ms()`를 함께 본다.

4. **chunked는 raw relay**
   - `chunked.c`는 framing 검증과 decoded byte 수 계산을 한다.
   - 프록시는 chunk를 재인코딩하지 않고 원문 byte를 그대로 전달한다.
   - 정적/CGI route는 chunked 요청을 `501`로 거절한다.

5. **`max_conns`는 client만 센다**
   - 프록시 연결은 upstream fd가 추가로 필요하다.
   - 동시 프록시 연결은 `max_upstreams`로 별도 제한한다.

6. **`server`와 `server_ev`는 같은 코드**
   - Makefile에서 같은 오브젝트를 링크한다.
   - `server_ev`는 Phase 2 이벤트 루프 바이너리 이름의 흔적이다.

---

## 12. 더 찾아보기

- [README](../README.md) — 발표용 전체 구조와 실행 방법
- [phase3-design.md](phase3-design.md) — 프록시 설계, 상태 머신, timeout, pool
- [phase2-design.md](phase2-design.md) — 이벤트 루프와 task pool
- [quality-gate.md](quality-gate.md) — 테스트 결과와 알려진 한계
