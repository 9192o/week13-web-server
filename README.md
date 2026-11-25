# week13 web server

C11로 구현한 HTTP/1.1 웹서버와 리버스 프록시입니다. 외부 웹서버나 이벤트 루프 라이브러리 없이 소켓, HTTP 파싱, 정적 파일 서빙, CGI, 프록시를 직접 구현했습니다.

- **Phase 1**: HTTP/1.1 파싱, 정적 파일 `GET`/`HEAD`, `POST`, CGI
- **Phase 2**: `poll`/`epoll`/`kqueue` 이벤트 루프, keep-alive, task pool 기반 CGI, graceful shutdown, access log
- **Phase 3**: 경로 기반 리버스 프록시, 라운드로빈, backend keep-alive pool, passive health, chunked stream-through

## 주요 기능

- 정적 파일 서빙: `GET`/`HEAD`, `Range`, `If-Modified-Since`, 디렉터리 redirect
- CGI: `/cgi-bin/` 경로, `posix_spawn` + task pool, timeout/종료 처리
- HTTP/1.1 keep-alive, 요청 크기 제한, `Expect: 100-continue`
- 프록시: longest-prefix 라우팅, backend별 라운드로빈, connect 실패 시 다음 backend 재시도
- backend keep-alive pool: `max_idle`, `idle_timeout`, stale 연결 감지
- passive health: connect 실패 backend를 일정 시간 건너뜀
- chunked 요청/응답 stream-through, gzip은 디코딩하지 않고 그대로 전달
- 프록시 타임아웃: 요청 body `408`, 응답/body `504`, backend 연결 실패 `502`
- `max_upstreams` 초과 시 `503` + `Retry-After: 1`
- route별 `strip_prefix`, INI 설정 파일 + CLI override

## 빌드

Linux 또는 macOS에서:

```sh
make
```

`server`와 `server_ev`가 같은 소스로 빌드됩니다. 프록시 테스트용 backend는 별도로 빌드합니다.

```sh
make backend
```

## 실행

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

예시 설정 `proxy.conf`:

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

백엔드 두 개와 프록시를 실행합니다.

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

## 설정 파일

`#` 주석과 빈 줄을 허용하는 `key value` 형식입니다. `[global]` 섹션은 route 밖의 전역 설정으로 돌아갑니다.

| 키 | 의미 |
| --- | --- |
| `listen` | listen 포트 |
| `docroot` | 정적 파일 루트 |
| `cgi_dir` | CGI 실행 파일 디렉터리 (`/cgi-bin/` 요청과 연결) |
| `max_conns` | 동시 client 연결 상한 |
| `max_upstreams` | 동시 proxy 연결 상한, 초과 시 `503` |
| `task_workers` / `task_queue` | CGI 작업 스레드 수 / 대기 큐 크기 |
| `keepalive_idle` / `keepalive_max` | client keep-alive idle 초 / 최대 요청 수 |
| `max_uri` / `max_header` / `max_body` | URI·헤더·본문 한도 (byte) |
| `timeout request` | client 요청 body 대기 시간 (초, 초과 시 `408`) |
| `timeout connect` | backend connect 시간 (초) |
| `timeout response` | backend 응답 헤더 대기 시간 (초, 초과 시 `504`) |
| `timeout body` | backend 응답 body idle 시간 (초, 초과 시 `504`) |
| `pool max_idle` | backend별 idle 연결 최대 개수 |
| `pool idle_timeout` | idle 연결 유지 시간 (초) |
| `[route <prefix>]` | route 섹션 시작 |
| `type static\|proxy` | route 종류 |
| `backend host:port` | proxy backend (여러 번 사용 가능) |
| `strip_prefix 0\|1` | upstream 요청 경로에서 route prefix 제거 |

경로가 여러 route와 일치하면 가장 긴 prefix가 선택됩니다. `/api/`는 `/apix`와 매칭되지 않습니다.

## CLI

```text
./server <port> <docroot> [cgi_dir] [options]
./server --config <file> [options]
```

주요 옵션:

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

## 테스트

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

현재 로컬 기준 `make test`는 **358 assertions**, 전체 integration은 **182 PASS**입니다. GitHub Actions의 Linux CI에서 빌드, 전체 integration, sanitize, load, proxy-load를 확인합니다.

## 디렉터리

```text
src/        서버, HTTP 파서, 프록시, 이벤트 루프, CGI, 설정
include/    헤더
tests/      unit/integration 테스트, 테스트 backend, 부하 스크립트
docs/       Phase별 요구사항·설계·품질 문서
www/        정적 파일과 데모 페이지
cgi-bin/    CGI 예제 프로그램
```

## 범위 밖 / 알려진 한계

- TLS, HTTP/2, WebSocket, Forward Proxy, `CONNECT`는 지원하지 않습니다.
- 캐싱, gzip 변환, active health check는 범위 밖입니다. gzip은 pass-through만 합니다.
- backend hostname은 시작 시 한 번 resolve합니다. DNS 변경은 재시작해야 반영됩니다.
- 설정 파일의 IPv6 literal backend(`[::1]:8001`)는 지원하지 않습니다.
- `max_conns`는 client 연결만 세므로 프록시 사용 시 upstream fd가 추가로 필요합니다.
- `pool max_idle` 기본값이 8이라 동시 연결이 많으면 backend 연결이 여러 개 생길 수 있습니다.

## 문서

- [요구사항](docs/requirements.md)
- [아키텍처](docs/architecture.md)
- [Phase 1 요구사항](docs/phase1-requirements.md)
- [Phase 2 설계](docs/phase2-design.md)
- [Phase 3 설계](docs/phase3-design.md)
- [Phase 3 요구사항](docs/phase3-requirements.md)
- [품질 게이트](docs/quality-gate.md)
