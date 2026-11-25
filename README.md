# week13 web server

C11로 직접 구현한 HTTP/1.1 웹서버와 리버스 프록시입니다. 외부 웹서버나 이벤트 루프 라이브러리 없이 소켓, HTTP 파싱, 정적 파일 서빙, CGI, 프록시를 직접 구현했습니다.

- **Phase 1**: HTTP/1.1 파싱, 정적 `GET`/`HEAD`, `POST`, CGI
- **Phase 2**: `poll`/`epoll`/`kqueue` 이벤트 루프, keep-alive, CGI task pool, graceful shutdown, access log
- **Phase 3**: 경로 기반 리버스 프록시, 라운드로빈, backend keep-alive pool, passive health, chunked stream-through

## 한눈에 보기

| 문제 | 선택 | 구현 위치 |
| --- | --- | --- |
| 연결 동시성 | 연결당 스레드 대신 **단일 이벤트 루프 + non-blocking I/O** | `src/server_ev.c`, `src/event_*.c` |
| CGI가 이벤트 루프를 막는 문제 | **task pool + `posix_spawn`** | `src/task_pool.c`, `src/cgi.c`, `src/cgi_task.c` |
| 프록시 backend 연결 비용 | **backend별 keep-alive pool** | `src/proxy.c`, `src/proxy_pool.c` |
| 큰 chunked 응답 | **디코딩/재인코딩 없이 stream-through** | `src/proxy.c`, `src/chunked.c` |
| 잘못된 설정 | 시작 시 **fail-fast 검증** | `src/config.c`, `src/config_file.c` |
| Phase 3 검증 | unit + integration + sanitize + fd + load | `tests/` |

발표에서 크게 잡을 포인트는 **“이벤트 루프로 연결을 처리하고, CGI만 task pool로 분리했으며, 프록시는 backend 연결을 재사용한다”**입니다.

## 전체 구조

```text
클라이언트
   │
   ▼
┌──────────────────────────────────────────────┐
│ 이벤트 루프 (단일 스레드)                     │
│ server_ev.c + event_poll/epoll/kqueue.c      │
│                                              │
│  accept → conn_t → EV_READ/EV_WRITE 상태 머신 │
└───────┬──────────────────┬───────────────────┘
        │                  │
        │ 정적 요청         │ /cgi-bin/ 요청
        ▼                  ▼
 static_file.c       task_pool.c → cgi.c → CGI 프로세스
        │                  │
        │ proxy route      │
        ▼                  ▼
    proxy.c  ───────→  http_upstream.c / chunked.c
        │
        ▼
   backend 서버
```

- **이벤트 루프는 1개**입니다. 연결마다 스레드를 만들지 않고, fd별 상태 머신으로 읽기/쓰기를 진행합니다.
- **CGI만 별도 worker**로 보냅니다. worker 수는 `task_workers`(기본 4), 대기 큐는 `task_queue`(기본 64)입니다.
- **access log/logger**는 별도 스레드에서 처리합니다.
- `server`와 `server_ev`는 같은 오브젝트를 링크하는 빌드 별칭입니다.

## 요청 처리 흐름

1. `accept()` 후 `conn_t`를 만들고 `EV_READ`를 등록합니다.
2. 입력 버퍼에서 HTTP 요청 헤더를 파싱합니다. URI/헤더/본문 한도를 넘으면 각각 `414`/`431`/`413`으로 거절합니다.
3. `config_find_route()`로 **longest-prefix route**를 찾습니다.
4. route 종류에 따라 분기합니다.
   - `ROUTE_PROXY` → `proxy_start()`
   - `/cgi-bin/` → `submit_cgi()` → task pool
   - 그 외 → `static_file_prepare()` → 정적 응답
5. 응답을 `ST_WRITE` 상태로 전송합니다. keep-alive면 상태를 초기화하고, 아니면 연결을 닫습니다.
6. 이벤트 루프가 주기적으로 timeout, keep-alive 만료, 프록시 pool 만료를 정리합니다.

`conn_t`의 주요 상태는 `ST_READ`, `ST_READ_BODY`, `ST_WAIT_TASK`, `ST_PROXY`, `ST_WRITE`입니다.

## Phase 1: HTTP와 정적 파일

### HTTP 파싱

- `src/http_request.c`에서 요청 라인, 헤더, `Content-Length`, chunked 여부를 직접 파싱합니다.
- `Host` 중복, `Content-Length` 중복/충돌, `Content-Length` + `Transfer-Encoding` 동시 사용, control character, 비정상 메서드/버전을 거부합니다.
- `src/http_response.c`가 상태 줄, 헤더, 에러 HTML을 만듭니다.
- 정적/CGI 경로는 chunked 요청을 지원하지 않고 `501`로 거절합니다. 프록시 경로만 chunked 요청을 stream-through 합니다.

### 정적 파일

- `src/static_file.c`
- `realpath` 기준으로 docroot 밖으로 나가는 경로와 symlink escape를 차단합니다.
- `Range`, `If-Modified-Since`, `Last-Modified`, `Accept-Ranges`, 디렉터리 `301` redirect를 처리합니다.
- 파일은 fd에서 읽어 응답 버퍼로 streaming 합니다.

## Phase 2: 이벤트 루프와 CGI

### 이벤트 루프

- `src/event_backend.c`가 플랫폼에 따라 backend를 고릅니다.
  - Linux: `epoll`
  - macOS/BSD: `kqueue`
  - fallback: `poll`
- `src/server_ev.c`가 accept, read/write, timeout, connection sweep, shutdown을 담당합니다.
- 응답을 다 보내지 못하면 `EV_WRITE`로 전환하고, 입력이 밀리면 `EV_READ`를 조절하는 방식으로 backpressure를 겁니다.
- keep-alive는 `keepalive_idle`, `keepalive_max`로 제한합니다.

### CGI

- `src/cgi.c`: `posix_spawn`으로 CGI를 실행하고 stdin/stdout pipe를 연결합니다.
- `src/cgi_task.c`: CGI job을 실행하고 결과를 응답으로 변환합니다.
- `src/task_pool.c`: worker thread와 queue를 관리합니다. 큐가 가득 차면 `503` + `Retry-After`를 반환합니다.
- CGI 출력의 `Status`, `Content-Type`, 중복 헤더, 204/304/205 body 규칙을 검증합니다.

### 종료와 로그

- `SIGINT`/`SIGTERM`을 받으면 새 연결 수락을 멈추고 진행 중인 연결을 유예 시간 동안 기다립니다.
- 두 번째 신호가 오면 강제 종료합니다.
- `src/logger.c`는 로그를 큐에 넣고 별도 스레드에서 출력합니다.
- `src/access_log.c`는 요청 라인, 상태 코드, 바이트 수, 프록시 비고를 기록합니다.

## Phase 3: 리버스 프록시

프록시는 `src/proxy.c`의 상태 머신이 중심이고, backend keep-alive pool은 `src/proxy_pool.c`, 요청/응답 헤더 재작성은 `src/proxy_wire.c`에 분리되어 있습니다.

```text
요청 헤더 파싱
   → route 선택
   → backend 선택 (round-robin, down이면 skip)
   → pool에서 idle 연결 획득 또는 connect
   → 요청 헤더/본문 전송
   → upstream 응답 헤더 파싱
   → Content-Length / chunked / EOF body 중계
   → keep-alive면 pool에 반환, 아니면 close
```

### 라우팅

- INI의 `[route <prefix>]` 섹션으로 route를 만듭니다.
- 가장 긴 prefix가 이깁니다.
- `/api/`는 `/apix`와 매칭되지 않도록 prefix 경계를 확인합니다.
- `strip_prefix 1`이면 upstream 요청 경로에서 prefix를 제거하고 query는 유지합니다.

### backend 선택과 장애 처리

- route마다 backend 목록을 두고 **round-robin**으로 선택합니다.
- connect에 실패하면 아직 시도하지 않은 다음 backend로 재시도합니다.
- 실패한 backend는 `down_until_ms` 동안 건너뜁니다(passive health).
- 모든 backend가 실패하면 `502`입니다.
- active `/health` probe는 Phase 3 범위 밖입니다.

### keep-alive pool

- backend별로 idle fd를 저장합니다.
- `pool max_idle`(기본 8), `pool idle_timeout`(기본 30초)로 제한합니다.
- pool에서 꺼낼 때 `MSG_PEEK`로 stale 연결을 감지합니다.
- HTTP/1.0 응답이거나 `Connection: close`이면 pool에 넣지 않습니다.
- `max_upstreams`를 넘으면 `503` + `Retry-After: 1`을 반환합니다.

### 요청/응답 중계

- 요청:
  - 원래 path/query 유지 또는 `strip_prefix` 적용
  - `Host` 유지
  - `X-Forwarded-For`, `X-Forwarded-Proto`, `X-Forwarded-Host` 추가
  - hop-by-hop 헤더와 `Connection`에 이름이 적힌 헤더 제거
  - `Expect: 100-continue`는 프록시가 먼저 `100 Continue`를 보내고 처리
- 응답:
  - upstream 상태 코드와 reason phrase 유지
  - hop-by-hop 헤더 제거
  - `Content-Length` 응답은 길이 유지
  - chunked 응답은 **원문 그대로 stream-through**, trailer도 전달
  - `Connection: close`/EOF-delimited 응답은 끝까지 읽고 client를 닫음
  - `204`, `304`, `205`, `HEAD`는 body 없이 처리
- gzip은 디코딩하지 않고 그대로 전달합니다.

### 타임아웃과 에러 매핑

| 상황 | 응답 |
| --- | --- |
| 잘못된 요청 | `400` |
| client 요청 body idle | `408 Request Timeout` |
| 요청 body 초과 | `413 Payload Too Large` |
| URI 초과 | `414 URI Too Long` |
| 헤더 초과 | `431 Request Header Fields Too Large` |
| backend connect 실패 | `502 Bad Gateway` |
| `max_conns`/`max_upstreams`/task queue 초과 | `503 Service Unavailable` + `Retry-After` |
| backend 응답 헤더/body idle 초과 | `504 Gateway Timeout` |

## 구현 파일 맵

| 영역 | 핵심 파일 | 방식 |
| --- | --- | --- |
| 진입점 | `src/main_ev.c` | 설정 파싱 후 이벤트 서버 실행 |
| 설정 | `src/config.c`, `src/config_file.c` | CLI + INI, 시작 시 검증 |
| 이벤트 루프 | `src/server_ev.c`, `src/event_*.c` | non-blocking, 상태 머신 |
| HTTP | `src/http_request.c`, `src/http_response.c` | 직접 파서, 응답 빌더 |
| 정적 파일 | `src/static_file.c` | docroot confinement, Range, IMS |
| CGI | `src/cgi.c`, `src/cgi_task.c`, `src/task_pool.c` | `posix_spawn`, pipe, worker pool |
| 프록시 | `src/proxy.c`, `src/proxy_pool.c`, `src/proxy_wire.c`, `src/http_upstream.c`, `src/chunked.c` | route, pool, header rewrite, relay, parser |
| 로그/종료 | `src/access_log.c`, `src/logger.c`, `src/server_ev.c` | logger thread, graceful shutdown |

## 빌드와 실행

Linux 또는 macOS에서:

```sh
make
```

테스트용 backend는 별도로 빌드합니다.

```sh
make backend
```

### 정적 서버

```sh
./server 8080 ./www
```

CGI 디렉터리까지 지정하려면:

```sh
./server 8080 ./www ./cgi-bin
```

- `http://127.0.0.1:8080/`
- `http://127.0.0.1:8080/demo/`
- `http://127.0.0.1:8080/cgi-bin/echo?x=1`

### 리버스 프록시

`proxy.conf`:

```ini
listen 8080
docroot ./www

max_conns 1024
max_upstreams 512

timeout request 20
timeout connect 2
timeout response 10
timeout body 10

pool max_idle 8
pool idle_timeout 30

[route /api/]
type proxy
strip_prefix 1
backend 127.0.0.1:8001
backend 127.0.0.1:8002

[route /]
type static
```

backend 두 개와 프록시를 실행합니다.

```sh
./tests/backend 8001 1 &
./tests/backend 8002 2 &
./server --config proxy.conf
```

```sh
curl http://127.0.0.1:8080/api/id
# id=1 또는 id=2 (라운드로빈)
```

`/api/id`는 `strip_prefix 1`에 의해 backend에서 `/id`로 전달됩니다. `strip_prefix`를 빼면 backend가 `/api/id`를 그대로 받습니다.

## 설정과 CLI

### 설정 파일

`#` 주석과 빈 줄을 허용하는 `key value` 형식입니다. `[global]`은 route 밖의 전역 설정으로 돌아갑니다.

| 그룹 | 키 |
| --- | --- |
| 서버 | `listen`, `docroot`, `cgi_dir`, `max_conns`, `max_upstreams` |
| HTTP | `max_uri`, `max_header`, `max_body`, `keepalive_idle`, `keepalive_max` |
| CGI | `task_workers`, `task_queue` |
| 타임아웃 | `timeout request`, `timeout connect`, `timeout response`, `timeout body` |
| 프록시 pool | `pool max_idle`, `pool idle_timeout` |
| route | `[route <prefix>]`, `type static\|proxy`, `backend host:port`, `strip_prefix 0\|1` |

### CLI

```text
./server <port> <docroot> [cgi_dir] [options]
./server --config <file> [options]
```

```text
--config <file>
--timeout <sec>
--max-uri <bytes>
--max-header <bytes>
--max-body <bytes>
--keepalive-idle <sec>
--keepalive-max <count>
--task-workers <count>
--task-queue <count>
--max-conns <count>
--max-upstreams <count>
```

CLI 값이 설정 파일보다 우선합니다.

## 테스트와 검증

```sh
make test              # unit tests
make integration       # 정적/CGI/keep-alive/에러 경로
make integration-s1    # Phase 2 이벤트 루프
make integration-s2    # backpressure
make integration-p3    # Phase 3 프록시
make shutdown-test     # SIGINT/SIGTERM graceful shutdown
make sanitize          # ASan/UBSan 전체 스위트
make fdtest            # fd 누수 검사
make load              # 정적 서버 부하 테스트
make proxy-load        # 프록시 부하 테스트 (ab 필요)
```

현재 로컬 기준 `make test`는 **358 assertions**, 전체 integration은 **182 PASS**입니다.

- Linux CI: 빌드, 전체 integration, sanitize, load, proxy-load 통과
- proxy `ab -k -n 10000 -c 100`: `failed=0`, 로컬 약 10k rps
- fd 누수: 9 → 9

## 범위 밖 / 알려진 한계

- TLS, HTTP/2, WebSocket, Forward Proxy, `CONNECT`는 지원하지 않습니다.
- 캐싱, gzip 변환, active health check는 범위 밖입니다. gzip은 pass-through만 합니다.
- backend hostname은 시작 시 한 번 resolve합니다. DNS 변경은 재시작해야 반영됩니다.
- 설정 파일의 IPv6 literal backend(`[::1]:8001`)는 지원하지 않습니다.
- `max_conns`는 client 연결만 세므로 프록시 사용 시 upstream fd가 추가로 필요합니다.
- `pool max_idle` 기본값이 8이라 동시 연결이 많으면 backend 연결이 여러 개 생길 수 있습니다.

## 문서

- [함수 흐름](docs/function_flow.md)
- [아키텍처](docs/architecture.md)
- [Phase 1 요구사항](docs/phase1-requirements.md)
- [Phase 2 설계](docs/phase2-design.md)
- [Phase 3 설계](docs/phase3-design.md)
- [Phase 3 요구사항](docs/phase3-requirements.md)
- [품질 게이트](docs/quality-gate.md)
