# Standalone reverse-proxy experiment

`ldc`의 프록시 코어를 별도 실행 파일로 빼보는 실험입니다. 실제 배포용이 아니라
"통합 서버에서 프록시만 분리하면 무엇이 남고 무엇이 필요한가"를 확인하기 위한
브랜치입니다.

실험은 두 단계로 진행했습니다.

1. `exp/standalone-proxy` — `server_ev.c`의 프록시 관련 부분을 복사해
   `reverse-proxy` 바이너리를 만든 단계
2. `exp/shared-conn` — 연결 수명주기를 `conn_common.c`로 뽑아
   `server`와 `reverse-proxy`가 **같은 코드**를 쓰도록 바꾼 단계

## 최종 결과 (exp/shared-conn)

- 새 실행 파일: `reverse-proxy`
- 새 파일:
  - `src/main_proxy.c` — `--config`만 받는 프록시 전용 진입점
  - `src/proxy_server.c` — 프록시 전용 이벤트 루프 (270줄)
  - `src/conn_common.c` — 두 서버가 공유하는 연결 수명주기 (654줄)
  - `include/conn_common.h`
  - `include/proxy_server.h`
  - `tests/integration_proxy_bin.sh` — 프록시 전용 통합 테스트
- 공유하는 것:
  - accept, `conn_t` 생성/해제/종료, read/write 상태 머신
  - header read, 408/414/431 에러 응답
  - keep-alive 유지와 파일 전송 루프
  - timeout, drain, signal 처리, access log
- 재사용:
  - `src/proxy.c`, `src/proxy_wire.c`, `src/proxy_pool.c`
  - `src/http_request.c`, `src/http_response.c`, `src/http_upstream.c`,
    `src/chunked.c`
  - `src/config.c`, `src/config_file.c`
  - `src/event_*.c`, `src/logger.c`, `src/access_log.c`, `src/util.c`
- 제외:
  - `src/static_file.c`, `src/cgi.c`, `src/cgi_task.c`, `src/task_pool.c`

`reverse-proxy` 바이너리에는 `static_file_prepare`, `submit_cgi`,
`task_pool_create` 심볼이 없습니다. 크기는 macOS 빌드 기준
`server` 205KB → `reverse-proxy` 165KB.

### 코드 변화

| 파일 | 이전 | 이후 |
| --- | ---: | ---: |
| `src/server_ev.c` | 1320줄 | 693줄 |
| `src/proxy_server.c` | 826줄 (복사본) | 270줄 |
| `src/conn_common.c` | 없음 | 654줄 |

`server_ev.c`와 `proxy_server.c`에서 중복되던 연결 수명주기가
`conn_common.c`로 모였습니다. 두 이벤트 루프는 여전히 별도지만,
이제 차이는 **요청 처리 방식(정적/CGI vs 프록시)** 과 **task pool 유무**뿐입니다.

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

## 남은 중복

- 이벤트 루프 dispatch는 아직 두 벌이다. `server`는 task pool completion fd와
  CGI를 처리하고, `reverse-proxy`는 프록시만 처리한다.
- `process_buffer`는 서버가 정적/CGI/proxy 라우팅을 하고, 프록시는 proxy만
  처리하므로 공유하지 않았다.
- CGI 요청 body를 읽는 `ST_READ_BODY` 경로는 서버에만 있다.

즉 **연결 수명주기는 공유**했지만, **애플리케이션 계층(요청 처리)** 은
의도적으로 분리되어 있다. 이 정도가 "실행 파일 분리"와 "코드 공유"의
현실적인 경계로 보인다.

## 한계

- `proxy_only` 플래그는 `config_t`에 실험용으로 추가했다.
- `conn_common.c`는 `proxy_detach`/`proxy_next_timeout_ms`를 알고 있어
  완전히 프록시와 무관한 모듈은 아니다. 정적 전용 서버와 공유하려면
  이 의존성을 콜백으로 빼야 한다.
- 이 브랜치는 실제 제품/과제 제출용이 아니라 구조 확인용이다.
