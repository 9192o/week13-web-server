# Standalone reverse-proxy experiment

`ldc`의 프록시 코어를 별도 실행 파일로 빼는 실험이며, 현재 브랜치의 기본 배포 구조(`client → reverse-proxy → backend-server`)의 기반입니다. 실제 배포용이 아니라
"통합 서버에서 프록시만 분리하면 무엇이 남고 무엇이 필요한가"를 확인하기 위한
브랜치입니다.

실험은 두 단계로 진행했습니다. 두 단계 모두 현재
`ldc-standalone-proxy` 브랜치에 들어 있습니다.

1. 1차 — `server_ev.c`의 프록시 관련 부분을 복사해 `reverse-proxy`
   바이너리를 만든 단계
2. 2차 — 연결 수명주기를 `conn` 계층으로 뽑아 `server`와
   `reverse-proxy`가 **같은 코드**를 쓰도록 바꾼 단계

## 최종 결과 (ldc-standalone-proxy)

- 새 실행 파일: `reverse-proxy`
- 새 파일:
  - `src/app/main_proxy.c` — `--config`만 받는 프록시 전용 진입점
  - `src/app/proxy_server.c` — 프록시 전용 앱 어댑터 (147줄)
  - `src/conn/conn.c` — conn 생성/해제/로그/에러 응답
  - `src/conn/conn_io.c` — header read, read/write 상태 머신
  - `src/conn/conn_net.c` — listener, accept, 503 거절
  - `src/conn/conn_signal.c` — SIGINT/SIGTERM, stop flag
  - `src/conn/conn_timer.c` — timeout, drain, watchdog
  - `src/conn/conn_loop.c` — 두 서버가 공유하는 이벤트 루프 (223줄)
  - `src/app/server_ev.c` — 서버 앱 어댑터 (128줄)
  - `src/app/server_request.c` — 정적/CGI/proxy 라우팅 (301줄)
  - `src/app/server_cgi.c` — CGI task 제출/완료 (133줄)
  - `include/conn_common.h`, `include/conn_loop.h`, `src/conn/conn_internal.h`,
    `src/app/server_internal.h`
  - `include/proxy_server.h`
  - `tests/integration_proxy_bin.sh` — 프록시 전용 통합 테스트
- 공유하는 것:
  - accept, `conn_t` 생성/해제/종료, read/write 상태 머신
  - header read, 408/414/431 에러 응답
  - keep-alive 유지와 파일 전송 루프
  - timeout, drain, signal 처리, access log
  - 이벤트 루프 dispatch, listener/signal 관리, task-pool completion fd 연결
- 재사용:
  - `src/proxy/proxy_route.c`, `src/proxy/proxy_connection.c`, `src/proxy/proxy_relay.c`, `src/proxy/proxy_wire.c`, `src/proxy/proxy_pool.c`
  - `src/http/http_request.c`, `src/http/http_response.c`, `src/http/http_upstream.c`,
    `src/http/chunked.c`
  - `src/common/config.c`, `src/common/config_file.c`
  - `src/event/event_*.c`, `src/common/logger.c`, `src/common/access_log.c`, `src/common/util.c`
- 제외:
  - `src/static/static_file.c`, `src/cgi/cgi.c`, `src/cgi/cgi_task.c`, `src/cgi/task_pool.c`

`reverse-proxy` 바이너리에는 `static_file_prepare`, `submit_cgi`,
`task_pool_create` 심볼이 없습니다. 크기는 macOS 빌드 기준
`server` 209KB → `reverse-proxy` 169KB.

### 코드 변화

| 파일 | 이전 | 이후 |
| --- | ---: | ---: |
| `src/app/server_ev.c` | 1320줄 | 128줄 |
| `src/app/server_request.c` | 없음 | 301줄 |
| `src/app/server_cgi.c` | 없음 | 133줄 |
| `src/app/proxy_server.c` | 826줄 (복사본) | 147줄 |
| `src/conn/*.c` (5개로 분리) | 초기 `conn_common.c` 662줄 | 698줄 |
| `src/conn/conn_loop.c` | 없음 | 223줄 |

`server_ev.c`는 이제 앱 어댑터만 남았고, 정적/CGI 라우팅과 CGI 처리는
`server_request.c` / `server_cgi.c`로 나뉘었습니다. 연결 수명주기도
`conn.c`, `conn_io.c`, `conn_net.c`, `conn_signal.c`, `conn_timer.c`로
책임별로 분리했습니다. 두 서버의 차이는 **요청 처리 방식(정적/CGI vs 프록시)**
뿐입니다.

## 실행

```sh
make reverse-proxy
./reverse-proxy --config /tmp/proxy.conf
```

`/tmp/proxy.conf`:

```text
listen 19090
timeout request 5
timeout connect 1
timeout response 2
timeout body 2
pool max_idle 4
pool idle_timeout 2

[route /api/]
type proxy
backend 127.0.0.1:19091
backend 127.0.0.1:19092

[route /strip/]
type proxy
strip_prefix 1
backend 127.0.0.1:19091
backend 127.0.0.1:19092
```

프록시 전용 모드에서는 `docroot`가 필요 없고, static 라우트가 있으면
시작 시 거부합니다.

## 검증

```sh
make proxy-bin-test
```

macOS 로컬 결과: 12 PASS (`/api/id`, 라운드로빈, 404, strip_prefix,
chunked, POST, Expect 100-continue, keep-alive, 504, 502).

전체 검증:

- `make test` — 358 assertions
- `make integration`/`-s1`/`-s2`/`-p3`/`shutdown-test`/`proxy-bin-test` — 194 PASS
- `make sanitize` — PASS
- `make proxy-bin-test` with ASAN/UBSAN — 12 PASS
- Linux CI — PASS (epoll, sanitizer, load 포함)

## 남은 결합

- `process_buffer`는 서버가 정적/CGI/proxy 라우팅을 하고, 프록시는 proxy만
  처리하므로 공유하지 않았다. 이건 애플리케이션 계층이라 의도적이다.
- CGI 요청 body를 읽는 `ST_READ_BODY` 경로는 서버에만 있다.
- `conn_common`은 이제 `proxy_*` 함수를 직접 호출하지 않고,
  `conn_common_hooks_t`의 `detach`/`next_timeout_ms` 콜백만 쓴다.
- `conn_loop`는 `conn_loop_app_t`의 콜백만 호출하고, `proxy_handle_*`를
  직접 알지 않는다.
- `conn_t`는 이제 `void *app_state`와 `ST_STREAM` 상태를 쓴다. 연결 코어는
  프록시 타입을 직접 알지 않고, 앱이 `app_state`를 해석한다.

즉 **연결 수명주기와 이벤트 루프는 공유**했고, **애플리케이션 계층만
분리**되어 있다. "실행 파일 분리 + 코드 공유" 기준으로는 사실상 완료다.

## 한계

- `server_mode`는 FULL / PROXY_ONLY / BACKEND로 나뉜다. `backend-server`는 BACKEND 모드로 빌드된다.
- `conn_t`는 `void *app_state`/`ST_STREAM`으로 일반화되어 공통 코어가
  프록시 타입을 직접 포함하지 않는다. 프록시 앱이 `app_state`를 캐스팅해 쓴다.
- 이 브랜치는 실제 제품/과제 제출용이 아니라 구조 확인용이다.
