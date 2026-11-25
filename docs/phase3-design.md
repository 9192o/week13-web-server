# Phase 3 설계 — Reverse Proxy

> 상위: [requirements.md](requirements.md), [phase3-requirements.md](phase3-requirements.md)
> Phase 2 설계: [phase2-design.md](phase2-design.md)
> 테스트 계획: 이 문서 §18

## 1. 목표와 범위

Phase 3는 Phase 1의 HTTP 파서/응답 빌더와 Phase 2의 이벤트 루프/작업 풀 위에 **Reverse Proxy**를 추가한다.

- `client → proxy:8080 → backend:8001/8002` 구조.
- 경로 prefix 기반 라우팅 (longest match).
- `/api/*`는 backend로, 그 외 정적/CGI는 프록시가 직접 서빙.
- Round-Robin + passive health check.
- backend별 keep-alive 연결 풀.
- `Transfer-Encoding: chunked` stream-through 중계.
- gzip은 해제하지 않고 그대로 중계.
- INI 유사 설정 파일, 시작 시 1회 DNS resolve.
- 502/504, hop-by-hop 헤더 제거, backpressure, graceful shutdown.

범위 제외:

- Forward Proxy, `CONNECT`, TLS 종료, HTTP/2/3.
- 캐싱 (future: 작은 메모리 LRU).
- gzip 압축 해제/재압축.
- active `/health` (future).
- HTTP pipelining, WebSocket/Upgrade.

## 2. 결정 요약

| ID | 결정 | 이유 |
|---|---|---|
| D3 | Reverse Proxy | 과제 예시(`/api/*` → 8001)와 로드 밸런싱에 적합 |
| D4 | 경로 기반 (longest prefix) | 테스트가 명확하고 `/etc/hosts`가 필요 없음 |
| D5 | 프록시 직접 정적 서빙 | Phase 2 정적 코드 재사용, 정적/동적 라우팅 시연 |
| D6 | INI 유사 설정 파일 | 파서가 단순하고 fail fast가 쉬움. CLI > 파일 > 기본값 |
| D8 | 캐싱 없음 | 정확성/무효화/Vary 범위가 큼. future로 작은 LRU |
| D16 | keep-alive 풀 (Phase 3 필수) | 연결 재사용으로 latency/throughput/backend 연결 수 개선 |
| D17 | passive 헬스체크 only | 타이머 없이 dead backend 회피. active `/health`는 future |
| D18 | 시작 시 1회 DNS resolve | hostname 허용, 요청 경로에서 이벤트 루프 블로킹 방지 |
| gzip | 그대로 중계 | zlib 불필요, `Content-Encoding`/`Content-Length` 보존 |
| chunked | stream-through | 큰 body 버퍼링 없이 원본 chunk 중계 |

## 3. 전체 구조

```text
client fd
   │  Phase 2 conn_t (ST_READ / ST_PROXY / ...)
   ▼
server_ev event loop
   ├─ route match
   │    ├─ static / local CGI → Phase 2 static_file / cgi
   │    └─ proxy route      → proxy_conn_t
   │                              │
   │                              ├─ backend 선택 (Round-Robin, passive health)
   │                              ├─ pool에서 idle 연결 획득 또는 새 connect
   │                              ├─ 요청 헤더/본문 전송
   │                              ├─ 응답 헤더 파싱
   │                              └─ 응답 본문 중계 (CL/chunked/close)
   ▼
client fd ← proxy_conn_t ← upstream fd
```

- 소켓 I/O는 모두 이벤트 루프가 한다. proxy도 예외가 아니다.
- CGI 작업 풀은 그대로 유지한다. proxy 라우트가 아닌 `/cgi-bin/*`은 Phase 2 CGI로 처리한다.
- 로거/access log/종료 절차도 Phase 2 것을 재사용한다.

## 4. 모듈과 파일

| 파일 | 역할 |
|---|---|
| `include/proxy.h`, `src/proxy/proxy.c` | 라우팅, backend 선택, proxy 상태 머신, 헤더/본문 중계 |
| `src/proxy/proxy_conn.h`, `src/proxy/proxy_wire.c` | 내부 proxy 타입, 요청/응답 헤더 재작성 |
| `src/proxy/proxy_pool.c`, `src/proxy/proxy_internal.h` | backend keep-alive pool (idle fd, stale 검사, sweep) |
| `include/http_upstream.h`, `src/http/http_upstream.c` | upstream 응답(status line + headers) 파서 |
| `include/chunked.h`, `src/http/chunked.c` | chunked stream-through 파서 |
| `src/common/config_file.c` | INI 유사 설정 파일 파서 |
| `include/server_conn.h`, `include/ev_source.h` | client `conn_t`/`ST_PROXY`, event source kind |
| `tests/backend.c`, `tests/integration_p3.sh` | 테스트용 C backend와 Phase 3 통합 테스트 |
| `src/app/server_ev.c` | client 상태에 `ST_PROXY` 추가, route 분기, proxy 이벤트 dispatch |
| `include/config.h`, `src/common/config.c` | route/backend/timeout/pool 설정 필드 추가 |
| `Makefile` | proxy 모듈/테스트 backend/`integration-p3` 타깃 추가 |

## 5. 데이터 구조

### 5.1 route와 backend

```c
typedef enum {
    ROUTE_STATIC,
    ROUTE_PROXY
} route_type_t;

typedef struct {
    char host[256];              /* 설정에 적힌 값 (로그용) */
    int port;
    struct sockaddr_storage addr; /* 시작 시 resolve한 주소 */
    socklen_t addr_len;
    int down_until_ms;            /* passive health */

    proxy_idle_t *idle_head;      /* keep-alive 풀 */
    int idle_count;
} backend_t;

typedef struct route {
    char prefix[256];             /* "/api/" */
    route_type_t type;
    backend_t *backends;
    int backend_count;
    int rr_index;                 /* Round-Robin */
    struct route *next;
} route_t;
```

- route는 prefix 길이 내림차순으로 정렬하거나, 매칭 때 가장 긴 prefix를 고른다.
- `ROUTE_PROXY`인데 backend가 0개면 설정 오류로 시작 실패.
- `ROUTE_STATIC`은 Phase 2 static/CGI로 위임한다.
- route가 하나도 매칭되지 않으면 정적 서빙 → 404.

### 5.2 proxy 연결 상태

```c
typedef enum {
    PROXY_CONNECTING,       /* non-blocking connect 진행 중 */
    PROXY_SENDING,          /* 요청 헤더/본문 전송 */
    PROXY_READING_HEADERS,  /* upstream status line + headers */
    PROXY_RELAY_BODY,       /* 응답 본문 중계 */
    PROXY_DONE
} proxy_state_t;

typedef struct proxy_conn {
    int kind;                    /* EV_SRC_UPSTREAM */
    conn_t *client;              /* client conn_t */
    int upstream_fd;
    proxy_state_t state;
    backend_t *backend;
    route_t *route;

    char *req_out;               /* 요청 라인 + 필터링된 헤더 */
    size_t req_out_len;
    size_t req_out_sent;

    char *resp_out;              /* 상태 라인 + 필터링된 헤더 */
    size_t resp_out_len;
    size_t resp_out_sent;

    body_mode_t req_body;        /* NONE / CONTENT_LENGTH / CHUNKED */
    size_t req_remaining;
    chunk_parser_t req_chunk;

    body_mode_t resp_body;       /* NONE / CONTENT_LENGTH / CHUNKED / CLOSE */
    size_t resp_remaining;
    chunk_parser_t resp_chunk;

    char c2u[PROXY_BUF_SIZE];    /* client → upstream */
    size_t c2u_len;
    size_t c2u_off;
    char u2c[PROXY_BUF_SIZE];    /* upstream → client */
    size_t u2c_len;
    size_t u2c_off;

    uint64_t deadline_ms;        /* connect/response/body timeout */
    int close_client;
    int close_upstream;

    struct proxy_conn *next;
} proxy_conn_t;
```

초기값:

- `PROXY_BUF_SIZE` 64KB.
- 헤더 최대 크기는 기존 `max_header_bytes`를 재사용.
- 요청/응답 body는 가능한 한 버퍼에 모으지 않고 흘려보낸다.

### 5.3 event source 구분

현재 `ev_event_t.ptr`은 client `conn_t *` 하나뿐이다. upstream fd도 같은 루프에 들어오므로 구분이 필요하다.

```c
typedef enum {
    EV_SRC_CLIENT,
    EV_SRC_UPSTREAM
} ev_source_kind_t;

typedef struct {
    ev_source_kind_t kind;
} ev_source_t;
```

- `conn_t`와 `proxy_conn_t`의 **첫 필드**를 `ev_source_t source;`로 둔다.
- event loop는 `ptr`을 `ev_source_t *`로 보고 kind에 따라 분기한다.
- listen/completion/signal fd는 지금처럼 `ptr == NULL`로 두고 fd 번호로 구분한다.

### 5.4 client 상태 확장

`conn_state_t`에 `ST_PROXY`를 추가한다.

```text
ST_READ        헤더 읽기 / keep-alive idle
ST_READ_BODY   CGI POST body 읽기
ST_WAIT_TASK   CGI 작업 대기
ST_PROXY       proxy 요청 진행 중 (client↔upstream 중계)
ST_WRITE       일반 응답 쓰기
```

- `conn_t`에 `proxy_conn_t *proxy`를 추가한다.
- `ST_PROXY`에서 client fd 이벤트는 `proxy_on_client_readable`/`proxy_on_client_writable`로 위임한다.
- `conn_close`는 `proxy_detach(c)`를 호출해 upstream과 풀 상태를 정리한다.

## 6. 요청 흐름

### 6.1 client 요청 파싱

1. `on_readable`이 헤더 끝까지 읽는다.
2. `process_buffer`가 `http_request_parse_buffer`로 요청을 파싱한다.
3. `/cgi-bin/`이면 기존 CGI 경로를 우선한다.
4. route table에서 longest-prefix 매칭:
   - `ROUTE_STATIC` → 기존 `static_file_prepare`/`conn_build_static`
   - `ROUTE_PROXY` → `proxy_start`
5. proxy 요청이면 `conn_t.state = ST_PROXY`, `conn_t.proxy` 설정.

### 6.2 proxy_start

1. route의 Round-Robin으로 backend 선택 (passive health에서 down이면 skip).
2. 요청 헤더를 `req_out`으로 직렬화한다.
   - request line: method + 원래 path. route에 `strip_prefix 1`이 있으면 prefix를 제거하고, 빈 path는 `/`로 바꾸고 query는 유지한다.
   - hop-by-hop 헤더 제거
   - `Host` 유지, `X-Forwarded-For`/`X-Forwarded-Proto`/`X-Forwarded-Host` 추가
   - `Expect: 100-continue`는 프록시가 직접 `100 Continue`를 client에 보내고 헤더는 제거
3. backend의 pool에서 idle 연결 획득:
   - 있으면 `PROXY_SENDING`
   - 없으면 `socket` + non-blocking `connect` → `PROXY_CONNECTING`
4. `proxy_conn_t`를 연결 리스트에 추가하고 upstream fd를 `ev_add(EV_WRITE)`.

### 6.3 connect

- `connect()`가 `EINPROGRESS`면 `EV_WRITE`를 기다린다.
- `EV_WRITE`에서 `getsockopt(SO_ERROR)` 확인.
- 성공: pool/health 상태 갱신, `PROXY_SENDING`.
- 실패: backend를 passive down 표시, 다른 backend로 재시도(요청 body를 아직 보내지 않았으므로 안전), 모두 실패하면 `502`.

### 6.4 요청 전송

- `req_out`을 `write()`로 전송. `EAGAIN`이면 `EV_WRITE` 유지.
- 헤더 전송 후 body:
  - `Content-Length`: `c->in`에 남은 body부터 전송하고, 부족하면 client `EV_READ`로 더 읽는다.
  - `chunked`: chunk parser로 경계를 확인하며 원본 chunk를 그대로 전송.
  - body 없음: 바로 응답 헤더 대기.
- client body를 upstream으로 보내는 동안 backpressure를 적용한다.

### 6.5 응답 헤더 읽기

- upstream `EV_READ`에서 status line + headers를 `resp_hdr` 버퍼에 누적한다.
- `http_upstream_parse`가 다음을 판정:
  - status code
  - `Content-Length` / `Transfer-Encoding: chunked` / body 없음 / close까지
  - `Connection: close` 여부
  - hop-by-hop 헤더
- 헤더가 `max_header_bytes`를 넘으면 `502`.
- 파싱 성공 시 client로 보낼 `resp_out`을 만든다.
  - status line 유지
  - hop-by-hop 헤더 제거
  - `Connection`은 client keep-alive 정책에 맞게 재설정
  - `Content-Length`/`Transfer-Encoding`은 body 모드에 맞게 유지/재설정
  - gzip 헤더는 그대로 둔다
- `resp_out`을 client에 전송하고 `PROXY_RELAY_BODY`.

### 6.6 응답 body 중계

- `NONE`: body 없음. 응답 완료.
- `CONTENT_LENGTH`: `resp_remaining`만큼 upstream→client로 흘려보낸다.
- `CHUNKED`: chunk parser로 경계를 확인하며 원본 chunk를 그대로 client에 전달한다. trailer도 정책에 따라 전달/제거.
- `CLOSE`: upstream EOF까지 읽어 client에 전달하고, client 응답도 `Connection: close`로 끝낸다.
- client write가 `EAGAIN`이면 upstream `EV_READ`를 잠시 제거한다 (backpressure).
- body가 끝나면:
  - 응답 body를 끝까지 읽었는지 확인
  - upstream이 keep-alive 가능하면 pool에 반납
  - 아니면 close
  - client keep-alive 가능하면 `ST_READ`로 복귀, 아니면 close
  - access log 기록

## 7. 라우팅과 로드 밸런싱

### 7.1 longest-prefix match

- route는 `/api/`, `/api/v1/`, `/static/`, `/`처럼 `/`로 시작한다.
- 매칭된 prefix 중 가장 긴 것을 선택한다.
- `/api/v1/users`는 `/api/v1/`이 있으면 그쪽, 없으면 `/api/`.
- 매칭 실패 시 정적 서빙 → 404.

### 7.2 Round-Robin

```c
backend = route->backends[route->rr_index % route->backend_count];
route->rr_index++;
```

- route별 인덱스. 이벤트 루프가 단일 스레드이므로 락 불필요.
- passive health에서 `down_until_ms > now`인 backend는 건너뛴다.
- 모든 backend가 down이면 `502` (health cooldown 후 재시도).

### 7.3 passive health

- connect 실패/timeout 시:
  ```c
  backend->down_until_ms = now_ms() + HEALTH_COOLDOWN_MS;
  ```
- cooldown 기본 1000ms.
- 성공하면 `down_until_ms = 0`.
- 별도 타이머/스레드 없음. 다음 요청에서 자연스럽게 회복된다.
- active `/health`는 future.

## 8. 설정 파일

### 8.1 형식

INI 유사, 줄 단위, `#` 주석.

```text
listen 8080
docroot ./www
cgi_dir ./cgi-bin
max_conns 1024
max_upstreams 512
task_workers 4
task_queue 64
keepalive_idle 5
keepalive_max 100

timeout request 5
timeout connect 2
timeout response 10
timeout body 10

pool max_idle 8
pool idle_timeout 30

[route /api/]
type proxy
backend 127.0.0.1:8001
backend 127.0.0.1:8002

[route /internal/]
type proxy
strip_prefix 1
backend 127.0.0.1:8003

[route /static/]
type static

[route /]
type static
```

### 8.2 규칙

- `[route <prefix>]` 섹션 하나가 route 하나.
- `type`은 `static` 또는 `proxy`.
- `backend`는 `host:port`. `proxy` route에 1개 이상 필요.
- `strip_prefix 1`이면 upstream request path에서 route prefix를 제거한다.
- `max_upstreams`는 동시에 살아 있는 proxy 연결 수 상한이다. 넘으면 `503` + `Retry-After: 1`.
- `timeout request`는 client 요청 body를 기다리는 시간이다. 초과하면 `408 Request Timeout`.
- host는 IPv4 또는 hostname. hostname은 시작 시 1회 resolve. IPv6 literal(`[::1]:8001`)은 지원하지 않는다.
- 같은 prefix 중복은 오류.
- 알 수 없는 key/section, 잘못된 숫자/포트는 줄 번호와 함께 fail fast.
- `[global]` 섹션은 이후 key를 전역 설정으로 되돌린다.
- `route /`는 기본 route. 없으면 정적 404.
- `/cgi-bin/`은 proxy route보다 우선해서 Phase 2 CGI로 처리한다.

### 8.3 우선순위

```text
CLI > 설정 파일 > 기본값
```

- `--config <path>` 추가.
- `./server --config phase3.conf`처럼 positional port/docroot 없이 실행 가능하게 한다.
- positional 인자가 있으면 파일 값을 override한다.
- `--max-upstreams N`, `--max-conns N`으로 파일 값을 override할 수 있다.
- 기존 Phase 1/2 CLI 옵션은 그대로 유지한다.

## 9. upstream keep-alive 풀

### 9.1 구조

```c
typedef struct proxy_idle {
    int fd;
    uint64_t idle_since_ms;
    struct proxy_idle *next;
} proxy_idle_t;
```

- `backend_t`마다 idle 연결 리스트를 둔다.
- pool은 **backend 단위**다. 다른 backend의 연결을 섞지 않는다.
- idle 연결은 event loop에 등록하지 않는다. 대신 acquire 시 상태를 확인한다.

### 9.2 acquire

1. `idle_head`에서 하나 꺼낸다.
2. `recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT)`:
   - `0`: backend가 닫음 → close, 새 연결
   - `>0`: 예상치 못한 데이터 → close, 새 연결
   - `-1/EAGAIN`: 정상 → 재사용
   - `-1/EINTR`: 재시도
3. 풀이 비었으면 새 `socket` + `connect`.

### 9.3 release

- 응답 body를 **끝까지 읽은 뒤에만** 반납한다.
- 다음 경우는 close:
  - `Connection: close`
  - 파싱 실패/에러 응답
  - body framing이 불완전
  - pool max idle 초과
- 아니면 `idle_head`에 push하고 `idle_since_ms = now`.

### 9.4 sweep

- 이벤트 루프의 `next_timeout` 근처에서 주기적으로 `pool_sweep(now)`.
- `now - idle_since_ms > idle_timeout`이면 close.
- shutdown 시 `pool_close_all`.

### 9.5 health 연동

- backend가 passive down이 되면 그 backend의 idle 연결을 모두 close.
- cooldown 후 새 연결로 재시도.

### 9.6 retry 규칙

- 요청을 **한 바이트도 보내지 않았을 때** pooled 연결 실패 → 새 연결로 1회 재시도.
- 요청 body를 보낸 뒤 실패 → `502` (POST 중복 위험).
- `connect` 실패 → 다음 backend로 재시도 가능.

## 10. 헤더 처리

### 10.1 hop-by-hop 제거

다음 헤더는 client→upstream, upstream→client 양쪽에서 제거한다.

```text
Connection
Keep-Alive
Proxy-Authenticate
Proxy-Authorization
TE
Trailer
Transfer-Encoding
Upgrade
```

- `Connection: foo, bar`가 있으면 `foo`, `bar` 헤더도 제거한다.
- `Transfer-Encoding`은 제거한 뒤, 프록시가 body 모드에 맞게 새로 설정한다.
- `Trailer`는 trailer를 전달할 때만 새로 설정한다.

### 10.2 요청 헤더

- `Host`: 원본 유지.
- `X-Forwarded-For`: 기존 값이 있으면 `, client_ip` append. 없으면 `client_ip`.
- `X-Forwarded-Proto: http`.
- `X-Forwarded-Host`: 원본 Host.
- `Expect: 100-continue`: 프록시가 client에 `100 Continue`를 보내고 upstream에는 전달하지 않는다.
- `Content-Length`/`Transfer-Encoding`: 원본 body framing에 맞게 유지/재설정.
- `Connection`: upstream 연결 정책에 맞게 설정.

### 10.3 응답 헤더

- status line 유지.
- hop-by-hop 제거.
- `Content-Length`/`Transfer-Encoding`: body 모드에 맞게 유지/재설정.
- gzip: `Content-Encoding`, `Content-Length`, `Vary` 유지.
- `Connection`: client keep-alive 정책에 맞게 설정.
- `Set-Cookie` 등 end-to-end 헤더는 그대로 전달.

### 10.4 보안

- 헤더 값의 CR/LF는 파서에서 거부한다.
- `Content-Length`와 `Transfer-Encoding`이 동시에 있으면 client 요청은 `400`, upstream 응답은 `502`.
- 중복 `Content-Length`가 다른 값이면 거부.
- `Transfer-Encoding`이 `chunked` 외 값이면 거부.

## 11. Body framing과 chunked

### 11.1 요청 body

| 모드 | 처리 |
|---|---|
| 없음 | 헤더 전송 후 응답 대기 |
| Content-Length | `c->in` 잔여 → client read → upstream write |
| chunked | chunk parser로 경계 확인, 원본 chunk 전송 |

- 요청 chunked는 Phase 1 parser가 현재 `501`이므로 Phase 3에서 `BODY_CHUNKED` 모드를 추가한다.
- chunk parser는 `SIZE → DATA → CRLF → TRAILER → DONE` 상태를 가진다.

### 11.2 응답 body

| 모드 | 처리 |
|---|---|
| 없음 (HEAD/204/304/1xx) | body 무시 |
| Content-Length | 정확히 N바이트 중계 |
| chunked | 원본 chunk 중계 |
| close | EOF까지 중계, client도 close |

### 11.3 chunked stream-through

- chunk size line을 파싱해 data 길이를 안다.
- size line + data + CRLF를 **원본 그대로** 출력 버퍼로 복사한다.
- `0\r\n` 이후 trailer를 읽고, trailer 전달 정책에 따라 client에 쓴다.
- `Transfer-Encoding: chunked`는 프록시가 새로 설정한다.
- `Content-Length`와 `Transfer-Encoding`이 충돌하면 거부한다.

### 11.4 gzip

- `Content-Encoding: gzip`은 body 모드와 무관하게 end-to-end 헤더로 취급한다.
- 압축 해제/재압축하지 않는다.
- `Content-Length`는 압축된 바이트 수 그대로 중계한다.
- 캐싱은 Phase 3에서 하지 않으므로 `Vary`는 그대로 전달만 한다.

## 12. Backpressure

proxy는 양방향이므로 한쪽이 느릴 때 다른 쪽 읽기를 멈춰야 한다.

- `c2u` 버퍼가 가득 차면 client `EV_READ` 제거.
- upstream write가 `EAGAIN`이면 client `EV_READ` 제거, upstream `EV_WRITE` 등록.
- `u2c` 버퍼가 가득 차면 upstream `EV_READ` 제거.
- client write가 `EAGAIN`이면 upstream `EV_READ` 제거, client `EV_WRITE` 등록.
- buffer가 비면 제거했던 `EV_READ`를 다시 등록한다.
- client가 끊기면 upstream close, proxy state free.
- upstream이 끊기면 client에 `502`를 보내거나, 이미 보낸 응답이면 close.

권장 초기값:

- relay buffer 64KB.
- header buffer는 `max_header_bytes`.
- `pool max_idle` 8, `pool idle_timeout` 30초.

## 13. 타임아웃과 에러 매핑

| 단계 | 기본값 | 실패 시 |
|---|---|---|
| connect | 2초 | 다음 backend 재시도, 모두 실패 시 `502` |
| response header | 10초 | `504` |
| body idle | 10초 | `504` 또는 close |
| client body read | `timeout request` (5초) | `408`/close |

- backend connect 실패 → `502 Bad Gateway`
- backend 응답 지연 → `504 Gateway Timeout`
- upstream malformed response → `502`
- client disconnect → upstream close, 결과 폐기
- pooled 연결 stale + 요청 미전송 → 새 연결로 1회 재시도
- 요청 전송 후 실패 → `502`

## 14. 정적/CGI와의 관계

- route type `static`은 Phase 2 `static_file_prepare`/`conn_build_static`을 그대로 호출한다.
- `/cgi-bin/`은 proxy route보다 우선해서 Phase 2 CGI로 처리한다.
- route `/`가 static이면 정적 파일/404.
- 즉 프록시 서버 하나가:
  - `/api/*` → backend
  - `/static/*`, `/` → 직접 정적
  - `/cgi-bin/*` → 로컬 CGI
  를 동시에 수행한다.

## 15. 로깅

- access log는 기존 형식을 유지한다.
  ```text
  ip "METHOD path" status body_bytes
  ```
- proxy 응답의 status/body bytes를 기록한다.
- backend 주소/에러는 `log_error`로 남긴다.
  ```text
  proxy: backend 127.0.0.1:8001 connect failed: Connection refused
  proxy: backend 127.0.0.1:8001 response timeout
  ```
- hop-by-hop 헤더 값은 로그에 남기지 않는다.
- pool acquire/release는 debug 로그 수준으로만.

## 16. 종료

Phase 2 drain을 확장한다.

1. SIGINT/SIGTERM → listen 중단, 새 proxy 요청 중단.
2. `ST_READ`/`ST_READ_BODY` client는 close.
3. `ST_PROXY` client는 5초 유예:
   - 진행 중 요청/응답을 마치게 한다.
   - 5초가 지나면 client/upstream close.
4. pool의 idle 연결은 즉시 close.
5. `task_pool_shutdown`, logger flush.
6. watchdog `_exit`.

## 17. 테스트 backend

`tests/backend.c`는 프록시 테스트용 작은 HTTP 서버다.

| endpoint | 동작 |
|---|---|
| `GET /echo` | method/path/query/headers/body를 text로 반환 |
| `GET /id` | backend id (`id=N`) |
| `GET /slow?ms=N` | N ms 후 200 |
| `GET /chunked` | chunked 응답 3개 |
| `GET /status?code=N` | 지정 status |
| `GET /close` | 응답 없이 close |
| `GET /big` | 큰 body |
| `GET /gzip` | gzip 압축 body + `Content-Encoding: gzip` |
| `GET /health` | 200 (active health future) |
| `GET /stats` | 요청 수/연결 수 (pool 테스트용) |

- `tests/backend <port> <id>` 형태로 실행.
- `SO_REUSEADDR`, non-blocking은 필수 아님. 테스트용이므로 단순 blocking loop로 충분.
- Makefile에 `tests/backend` 타깃 추가.

## 18. 테스트 계획

### 18.1 단위

- `tests/test_proxy_route.c`: longest-prefix, 중복 prefix, backend 선택, round-robin.
- `tests/test_chunked.c`: size/data/CRLF/trailer, partial input, 잘못된 chunk.
- `tests/test_http_upstream.c`: status line, headers, CL/TE 충돌, no-body status.
- `tests/test_proxy_pool.c`: acquire/release, max idle, idle timeout, stale close.
- `tests/test_config_file.c`: 정상/오류 설정, 줄 번호, CLI override.

### 18.2 통합

- `/api/echo` → backend의 method/path/body 확인.
- `/static/index.html` → proxy가 직접 서빙.
- Round-Robin: backend 2개, `/api/id` 응답이 번갈아 나옴.
- backend down → `502`.
- `/slow` + `timeout response 1` → `504`.
- hop-by-hop 헤더 제거: client가 `Connection: X`, `X: secret`을 보내도 backend가 못 봄.
- `X-Forwarded-For` 확인.
- POST Content-Length body 중계.
- chunked 요청/응답 중계.
- gzip pass-through: `Content-Encoding`/`Content-Length`/body 보존.
- keep-alive pool: `/stats`의 연결 수 < 요청 수.
- pool stale: backend가 idle 연결을 닫아도 다음 요청 성공.
- passive health: backend down 후 skip, cooldown 후 복귀.
- client disconnect/backpressure.
- config 오류 fail fast.
- `make sanitize`, `make fdtest`, `make load`에 proxy 경로 추가.

### 18.3 부하

- `ab -n 10000 -c 100` direct vs proxy 비교.
- pool 유무에 따른 RPS/latency/backend 연결 수 비교.
- `Failed requests: 0`, non-2xx 0.

## 19. 구현 순서

1. 설정 파일 parser + route table + 시작 시 DNS resolve.
2. `conn_t`에 `ST_PROXY`/event source kind 추가.
3. 기본 proxy: connect → 요청 헤더 전송 → 응답 헤더 파싱 → Content-Length body 중계 → close.
4. hop-by-hop 헤더/XFF/502/504/timeout.
5. 정적 route와 proxy route 공존.
6. chunked stream-through (요청/응답).
7. backpressure.
8. keep-alive 풀 + passive health.
9. 테스트 backend + 단위/통합/sanitize/fd/load.
10. 문서/README 갱신.

## 20. 위험과 열린 항목

- **event source 구분**: client/upstream fd를 잘못 구분하면 use-after-free/close 오류가 생기기 쉽다. `kind`를 첫 필드로 두고 dispatch를 단순화한다.
- **response parser**: request parser와 별개다. status line/헤더/CL/TE/no-body를 정확히 처리해야 한다.
- **chunked**: partial input, trailer, CL/TE 충돌이 주요 버그 지점이다.
- **backpressure**: 양방향 버퍼가 무한히 커지지 않도록 EV_READ pause/resume을 반드시 구현한다.
- **pool stale**: idle 연결이 backend에서 닫힌 경우 acquire 시 감지하고 새 연결로 대체한다.
- **request smuggling**: CL/TE 충돌, 중복 CL, 비정상 TE를 거부한다.
- **DNS**: 시작 시 resolve는 DNS 변경에 취약하다. 재시작 정책을 문서화한다.
- **Linux epoll**: Phase 2와 동일하게 CI에서 확인한다.
- **캐싱/active health/gzip transform**: Phase 3 범위에서 제외. future로 문서화.

## 21. 구현 상태 (2026-11-25)

- **구현 완료**: `src/proxy/proxy.c`, `src/proxy/proxy_conn.h`, `src/proxy/proxy_wire.c`, `src/proxy/proxy_pool.c`, `src/proxy/proxy_internal.h`, `src/http/http_upstream.c`, `src/http/chunked.c`, `src/common/config_file.c`, `include/proxy.h`, `include/http_upstream.h`, `include/chunked.h`.
- **통합**: `server`/`server_ev`가 최종 이벤트 루프 서버로 빌드되고, `--config`로 Phase 3 설정을 읽는다.
- **동작 확인**:
  - `/api/*` → Round-Robin backend, 그 외 → 정적 서빙.
  - hop-by-hop 헤더 제거, `X-Forwarded-For` 추가.
  - Content-Length/chunked 요청·응답 중계, gzip pass-through.
  - keep-alive 풀(재사용/만료/stale 감지), passive health(connect 실패 502 + cooldown).
  - connect/response/body timeout, 502/504. client request body timeout은 408.
  - client keep-alive, 1MB body, backpressure.
  - `strip_prefix 1` route, `max_upstreams` 상한(503 + `Retry-After`), upstream reason phrase/Trailer 보존.
- **검증**: `tests/test_chunked.c` 6개, `tests/test_http_upstream.c` 11개, `tests/test_config_file.c` 5개, `tests/integration_p3.sh` 42개 PASS. `make test` **358 assertions**, 전체 integration **182 PASS**, `make sanitize` PASS, fd 9→9. proxy `ab -k -n 10000 -c 100` failed=0, 약 8.7k rps (macOS 로컬, backend 2개).
- **실패 경로**: upstream EOF/EV_ERR를 읽기 후 판정(Connection: close, close-delimited, chunked+close), body idle timeout, 1xx+최종 동시 도착, 응답 Connection named header 제거, backend failover, deferred proxy free, 205/HEAD/204/304 no-body, HTTP/1.0 pool 금지, route prefix 경계, HTAB 허용, slow request body 408, upstream 상한 503.
- **알려진 한계**:
  - `pool max_idle` 기본 8이라 100 concurrent에서는 upstream 연결이 다수 생성된다.
  - `max_conns`는 client만 세므로 proxy 연결에서는 upstream fd가 2배 필요하다.
  - 설정 파일의 IPv6 literal(`[::1]:8001`)은 아직 지원하지 않는다.
  - `test_proxy_route.c`/`test_proxy_pool.c` 단위 테스트 대신 integration으로 검증한다.
  - active `/health`, 캐싱, gzip 변환, Forward proxy, CONNECT는 범위 밖.
  - Linux epoll CI는 통과했다.
