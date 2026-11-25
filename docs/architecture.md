# 아키텍처 — Phase 3

> 최종 `server`는 `src/app/main_ev.c` + `src/app/server_ev.c` 기반의 **이벤트 루프 + task pool** 서버다. 이 문서는 현재 코드의 구조, 모듈 경계, 데이터 소유권을 설명한다. 함수 호출 순서는 [function_flow.md](function_flow.md), 프록시 상세 설계는 [phase3-design.md](phase3-design.md)를 본다.

## 1. 설계 목표

- 연결마다 스레드를 만들지 않고 **단일 이벤트 루프**에서 non-blocking I/O로 처리한다.
- CGI처럼 block될 수 있는 작업만 **worker thread pool**로 분리한다.
- 정적 파일, CGI, 리버스 프록시를 하나의 서버 프로세스에서 처리한다.
- 프록시 backend 연결은 **keep-alive pool**로 재사용한다.
- 잘못된 설정은 시작 시 **fail-fast**로 거부한다.

Phase 1의 `server.c` 연결 루프는 Phase 2에서 이벤트 루프로 대체됐고, Phase 3에서 `proxy.c`가 추가됐다.

## 2. 실행 모델

```text
┌─────────────────────────────────────────────┐
│ main thread                                  │
│  - epoll / kqueue / poll                    │
│  - client 연결 상태 머신                     │
│  - 정적 파일 전송                            │
│  - 프록시 client/upstream 중계               │
└───────────────┬─────────────────────────────┘
                │ task queue
                ▼
┌─────────────────────────────┐
│ CGI task pool (기본 4 workers)│
│  posix_spawn + pipe + timeout│
└─────────────────────────────┘

┌─────────────────────────────┐
│ logger thread                │
│  access/error log queue      │
└─────────────────────────────┘
```

| 실행 단위 | 담당 |
| --- | --- |
| main thread | accept, HTTP 파싱, 정적 파일, 프록시, 응답 전송 |
| CGI worker | `posix_spawn`, CGI 입출력, timeout |
| logger thread | 로그 큐 소비 |

프록시는 별도 스레드가 아니라 **main event loop 안의 상태 머신**이다. 따라서 프록시 연결끼리 공유 상태를 lock으로 보호할 필요는 없지만, 한 연결의 긴 콜백이 전체 루프를 막을 수 있다.

## 3. 모듈 구조

```text
main_ev
├─ config
│  └─ config_file
└─ server_ev
   ├─ event_backend ─ event_epoll / event_kqueue / event_poll
   ├─ http_request
   ├─ http_response
   ├─ static_file
   ├─ cgi_task ─ task_pool
   ├─ proxy ─ http_upstream / chunked / proxy_pool / proxy_wire
   ├─ access_log
   └─ logger
```

| 모듈 | 책임 |
| --- | --- |
| `main_ev.c` | 진입점, 설정 로드, `server_ev_run()` |
| `config.c` / `config_file.c` | CLI/INI 설정, 검증, route 매칭 |
| `server_ev.c` | 이벤트 루프, `conn_t` 상태 머신, 정적/CGI/프록시 연결 |
| `event_*.c` | epoll/kqueue/poll abstraction |
| `http_request.c` | 요청 라인/헤더/URI/body 파싱 |
| `http_response.c` | 응답 헤더/에러 응답 생성 |
| `static_file.c` | docroot 매핑, MIME, Range, IMS |
| `cgi.c` / `cgi_task.c` | CGI 실행과 결과 변환 |
| `task_pool.c` | CGI worker queue와 completion fd |
| `proxy.c` | route/backend 선택, 프록시 상태 머신 |
| `proxy_conn.h` | 내부 `proxy_conn_t`/route/state/body 타입 |
| `proxy_wire.c` | 요청/응답 헤더 직렬화, hop-by-hop 제거 |
| `proxy_pool.c` | backend keep-alive pool (idle fd, stale 검사, sweep) |
| `http_upstream.c` | upstream 응답 헤더 파서 |
| `chunked.c` | chunked framing/trailer 파서 |
| `access_log.c` / `logger.c` | 요청 로그와 logger thread |

`server`와 `server_ev`는 같은 오브젝트를 링크하는 빌드 별칭이다.

## 4. 이벤트 소스와 연결 상태

### 두 종류의 source

`ev_event_t.ptr`은 `ev_source_t *`다.

```c
typedef struct {
    int kind;   /* EV_SRC_CLIENT or EV_SRC_UPSTREAM */
} ev_source_t;
```

- `conn_t`: client 연결. `source.kind = EV_SRC_CLIENT`
- `proxy_conn_t`: upstream 연결. `source.kind = EV_SRC_UPSTREAM`

두 struct 모두 `source`를 **첫 필드**로 둔다. 이벤트 루프는 `source.kind`로 어느 쪽인지 구분한 뒤 해당 handler로 넘긴다.

### client 상태

| 상태 | 의미 | 담당 |
| --- | --- | --- |
| `ST_READ` | 요청 헤더 읽기 | `on_readable()` |
| `ST_READ_BODY` | CGI용 body 읽기 | `on_readable()` |
| `ST_WAIT_TASK` | CGI worker 결과 대기 | completion fd |
| `ST_PROXY` | 프록시 중계 | `proxy_*` |
| `ST_WRITE` | 응답 전송 | `on_writable()` |

### 이벤트 루프의 책임 분리

- listen fd: `accept_new()`
- task completion fd: `drain_task_completions()`
- signal self-pipe: 종료 신호 소비
- `EV_SRC_UPSTREAM`: `proxy_handle_upstream_event()`
- `ST_PROXY` client: `proxy_handle_client_event()`
- 그 외 client: `on_readable()` / `on_writable()`

루프 끝에서 `conn_sweep()`, `expire_timers()`, `proxy_sweep()`, `proxy_reap()`를 호출한다. 일반 연결 deadline은 `expire_timers()`가, 프록시 deadline은 `proxy_sweep()`이 소유한다.

## 5. 요청 처리 경계

`process_buffer()`가 요청을 파싱한 뒤 세 갈래로 나눈다.

```text
process_buffer
├─ proxy route     → proxy_start
├─ /cgi-bin/       → submit_cgi
└─ static/OPTIONS  → static_file_prepare
```

### 버퍼 소유권

- `conn_t.in[16384]`: client 입력 버퍼
- `http_request_t`의 헤더 이름/값은 `c->in`을 가리키는 **zero-copy 포인터**다.
- `header_len`은 헤더 블록이 끝난 위치다.
- 정적/OPTIONS는 body를 소비하지 않는다. body가 남아 있으면 `close_after`로 닫는다.
- CGI는 body를 별도 buffer에 복사해 `cgi_job_t`로 넘긴다.
- 프록시는 body를 `c->in` 또는 socket에서 읽어 `c2u`로 옮기며 streaming 한다.
- keep-alive에서 남은 pipelined byte는 `memmove`로 앞으로 옮긴다.

### chunked 경계

- 프록시 route: chunked 요청을 **원문 그대로** upstream에 전달한다.
- 정적/CGI route: chunked 요청을 `501`로 거절한다.

## 6. 정적 파일

- `static_file_prepare()`가 docroot 검증, dotfile/symlink 탈출 차단, Range/IMS를 처리하고 `static_result_t`를 만든다.
- `conn_build_static()`이 응답 헤더를 만들고 `file_fd`, `file_remaining`, `file_offset`을 `conn_t`에 넣는다.
- 실제 전송은 event loop의 `on_writable()`이 `EV_FILE_CHUNK`(64KB) 단위로 처리한다.
- 파일 fd는 전송 완료/에러/close 경로에서 반드시 닫는다.
- 정적 파일은 CGI worker로 보내지 않는다. 디스크 read가 event loop에서 일어난다.

## 7. CGI

```text
process_buffer
└─ submit_cgi
   ├─ cgi_job_create (요청/body 소유권 이전)
   ├─ task_pool_submit
   ├─ state = ST_WAIT_TASK
   └─ client fd를 event loop에서 제거

worker
└─ cgi_job_execute
   ├─ posix_spawn
   ├─ stdin/stdout pipe
   ├─ timeout / signal 처리
   └─ cgi_result_t

completion fd
└─ drain_task_completions
   ├─ task_id로 conn_t 검색
   ├─ cgi_result_t의 response를 conn_t로 이동
   ├─ state = ST_WRITE
   └─ client fd를 EV_WRITE로 재등록
```

- worker 수와 queue 크기는 `task_workers`/`task_queue`.
- queue full이면 `503 + Retry-After`.
- `ST_WAIT_TASK` 동안 client fd는 이벤트 루프에 없으므로 client 이벤트가 발생하지 않는다.
- CGI 결과의 `response` 포인터는 `conn_t`로 소유권이 이동하고, `cgi_result_t`는 해제된다.

## 8. 프록시 구조

### 8.1 구성 요소

| 구성 | 역할 |
| --- | --- |
| `proxy_route_t[]` | route prefix와 backend 목록 |
| `proxy_backend_t` | host/port, resolved sockaddr, `down_until_ms`, idle pool |
| `proxy_conn_t` | 요청 하나의 프록시 상태와 buffer |
| `proxy_idle_t` | backend별 idle upstream fd |
| `proxy_wire.c` | 요청/응답 헤더 직렬화 |
| `proxy_pool.c` | idle fd 획득/반납/sweep |
| `http_upstream.c` | upstream 응답 헤더 파서 |
| `chunked.c` | chunked framing/trailer 파서 |

### 8.2 `proxy_conn_t` 소유권

내부 타입 정의는 `src/proxy/proxy_conn.h`에 있고, 요청/응답 헤더 직렬화는 `src/proxy/proxy_wire.c`가 담당한다.

```text
proxy_conn_t
├─ req_out       요청 헤더 직렬화 buffer
├─ c2u[64KB]     client → upstream body buffer
├─ resp_hdr      upstream 응답 헤더 buffer
├─ resp_out      client 응답 헤더 buffer
├─ u2c[64KB]     upstream → client body buffer
├─ req_chunk     요청 chunk parser
├─ resp_chunk    응답 chunk parser
├─ deadline_ms
└─ tried_mask
```

- `req_out`, `resp_out`, `resp_hdr`는 동적 할당이며 `proxy_reap()`에서 해제된다.
- `c2u`, `u2c`는 `proxy_conn_t`에 내장된 고정 크기 buffer다.
- `route`/`backend`는 전역 route table을 가리키며 `proxy_conn_t`가 소유하지 않는다.
- `proxy_free_struct()`는 즉시 free하지 않고 `dead` 표시 후 `g_dead`에 넣는다. 실제 free는 이벤트 배치가 끝난 뒤 `proxy_reap()`가 한다.

### 8.3 상태 전이

```text
PROXY_CONNECTING
   │ connect 성공
   ▼
PROXY_SENDING
   │ 요청 헤더 + body 전송 완료
   ▼
PROXY_READING_HEADERS
   │ 최종 응답 헤더 파싱
   ▼
PROXY_RELAY_BODY
   │ body 완료
   ▼
proxy_finish()
```

- `CONNECTING`: `EV_WRITE`에서 `SO_ERROR` 확인, 실패 시 다음 backend
- `SENDING`: `req_out`/`c2u` 전송, client body 읽기
- `READING_HEADERS`: upstream 헤더 파싱, 1xx skip
- `RELAY_BODY`: CL/chunked/EOF body 중계
- `finish`/`fail`/`abort`: pool 반환 또는 close, client keep-alive 복귀 또는 close

### 8.4 backend 선택과 health

- route별 round-robin cursor
- `down_until_ms`가 지나지 않은 backend만 후보
- connect 실패 시 `tried_mask`에 추가하고 다음 backend 시도
- 모든 backend가 실패하면 `502`
- active `/health` probe는 없다. passive cooldown만 있다.

### 8.5 pool

- backend별 idle fd list
- `pool max_idle` 기본 8, `pool idle_timeout` 기본 30초
- `proxy_pool_acquire()`는 `MSG_PEEK`로 stale 연결을 감지
- HTTP/1.0 응답 또는 `Connection: close`는 pool에 넣지 않음
- `proxy_pool_sweep()`이 idle timeout을 닫고, `proxy_next_timeout_ms()`가 pool 만료를 반영해 event loop를 깨운다

### 8.6 실패 정책

| 시점 | 정책 |
| --- | --- |
| 응답 시작 전 | `conn_build_error(status)`로 에러 응답 |
| 응답 시작 후 | 두 번째 응답을 쓰지 않고 연결 close |
| client disconnect | upstream close, proxy 정리 |
| upstream EOF/EV_ERR | 남은 데이터를 먼저 읽고, 불완전하면 `502` 또는 close |

프록시 경로에서는 `400`, `408`, `413`, `502`, `503`, `504`를 반환한다. 요청 파서 단계의 `414`/`431`은 프록시 진입 전에 처리된다.

## 9. 설정과 초기화

```text
config_init
  → config_parse_cli
      → config_parse_file (선택)
      → CLI override
  → config_validate
  → server_ev_run
      → proxy_init (backend DNS resolve)
      → event loop
```

- 설정 파일은 `key value`, `[route]`, `[global]` 형식이다.
- route 중복, backend 없는 proxy route, static route의 backend, IPv6 literal backend, 잘못된 숫자를 시작 시 거부한다.
- backend hostname은 시작 시 1회 resolve한다. 설정 hot reload는 없다.
- `max_conns`는 client 연결 수, `max_upstreams`는 활성 프록시 연결 수를 제한한다.

## 10. 종료와 자원

- `SIGINT`/`SIGTERM` 첫 신호: `g_stop`, self-pipe wake
- 두 번째 신호: `_exit(1)`
- `begin_drain()`: listen fd close, `ST_READ`/`ST_READ_BODY` close, 나머지는 `close_after`
- 최대 5초 drain, watchdog thread가 초과 시 종료
- `proxy_shutdown_all()`: active proxy와 pool fd 정리
- `task_pool_shutdown()`: worker 종료와 pending result 정리
- `proxy_cleanup()`: route pool close, dead proxy reap
- `logger_stop()`: 로그 큐 flush 후 thread 종료

## 11. 테스트 구조

| 계층 | 내용 |
| --- | --- |
| unit | HTTP parser, response, static, CGI, chunked, upstream, config |
| integration | 정적/CGI/keep-alive/에러 경로 |
| integration-s1 | 이벤트 루프/동시성 |
| integration-s2 | `max_conns`, task queue backpressure |
| integration-p3 | 프록시 route/pool/state machine/timeout |
| shutdown | SIGINT/SIGTERM drain |
| sanitize | ASan/UBSan 전체 스위트 |
| fdtest | fd 누수 |
| load / proxy-load | `ab` 부하 |

프록시는 unit test 없이 integration-p3에 의존한다. 이벤트 순서와 partial I/O를 전부 증명하는 테스트는 아니므로, 프록시를 수정할 때는 integration-p3와 sanitize를 반드시 다시 돌려야 한다.

## 12. 설계 trade-off

| 선택 | 장점 | 비용 |
| --- | --- | --- |
| 단일 event loop | 연결별 lock/thread 없음, 구조 단순 | 긴 콜백이 전체 연결을 막음 |
| CGI task pool | blocking CGI를 event loop에서 분리 | worker/queue 관리, 결과 전달 복잡 |
| 정적 파일 event loop 처리 | fd 소유권 단순 | 디스크 read가 loop를 막을 수 있음 |
| 고정 64KB body buffer | 메모리 상한, backpressure 구현 쉬움 | 큰 body는 여러 번 나눠 처리 |
| backend keep-alive pool | TCP handshake 감소 | stale 검사, idle timeout, pool 크기 관리 |
| 시작 시 DNS resolve | 요청 경로 단순 | DNS 변경에 재시작 필요 |
| passive health | 구현 단순 | 죽은 backend를 매 요청 재시도할 수 있음 |
| 프록시 통합 | 정적/CGI와 한 프로세스 | `proxy.c`/`proxy_pool.c`가 커지고 상태 머신 결합 |

## 13. 더 찾아보기

- [README](../README.md) — 빌드/실행/발표용 요약
- [function_flow.md](function_flow.md) — 함수 호출 순서
- [phase2-design.md](phase2-design.md) — 이벤트 루프/task pool 설계
- [phase3-design.md](phase3-design.md) — 프록시 상세 설계
- [quality-gate.md](quality-gate.md) — 검증 결과와 한계
