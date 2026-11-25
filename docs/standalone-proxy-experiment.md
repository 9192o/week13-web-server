# Standalone reverse-proxy experiment

`ldc`의 프록시 코어를 별도 실행 파일로 빼보는 실험입니다. 실제 배포용이 아니라
"통합 서버에서 프록시만 분리하면 무엇이 남고 무엇이 필요한가"를 확인하기 위한
브랜치입니다.

## 결과

- 새 실행 파일: `reverse-proxy`
- 새 파일:
  - `src/main_proxy.c` — `--config`만 받는 프록시 전용 진입점
  - `src/proxy_server.c` — 정적/CGI/task pool을 뺀 프록시 전용 이벤트 루프
  - `include/proxy_server.h`
  - `tests/integration_proxy_bin.sh` — 프록시 전용 통합 테스트
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
`server` 204KB → `reverse-proxy` 164KB.

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
chunked, POST, Expect 100-continue, keep-alive, 504, 502). 기존 `make test` 358 assertions와
`make integration-p3` 42 PASS도 그대로 통과합니다.

## 발견한 것

- 프록시 코어는 이미 `proxy_wire`/`proxy_pool`로 꽤 분리되어 있어서
  실행 파일 분리 자체는 가능합니다.
- 하지만 연결 수명주기(accept, read/write 상태 머신, timeout, drain)는
  `server_ev.c`에 강하게 붙어 있습니다. 이번 실험은 그 부분을
  `proxy_server.c`로 **복사해서 줄인** 것이지, 공통 모듈로 뽑은 것이 아닙니다.
- 진짜 분리를 하려면 `conn_t`와 클라이언트 read/write 상태 머신을
  `server_conn.c` 같은 공통 모듈로 옮기고, 통합 서버와 프록시 서버가
  그 모듈을 공유하도록 바꿔야 합니다.
- 그 작업은 `server_ev.c`의 정적/CGI 경로와 프록시 경로를 동시에 건드리므로,
  이 실험보다 2~4배 큰 리팩터링입니다.

## 한계

- `proxy_server.c`는 `server_ev.c`의 프록시 관련 부분을 복사한 코드다.
  두 파일이 갈라질 수 있으므로 유지보수용으로는 좋지 않다.
- `proxy_only` 플래그는 `config_t`에 실험용으로 추가했다.
- 이 브랜치는 실제 제품/과제 제출용이 아니라 구조 확인용이다.
