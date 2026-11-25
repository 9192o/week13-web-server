# week13-web-server

TCP 연결 수락부터 HTTP/1.1 응답 전송까지 C17과 Linux 소켓 API로 구현한 크래프톤 정글 13주차 프로젝트입니다.

**목차:** [개요](#overview) · [요구 사항](#requirements) · [개발 환경](#development) · [서버 구현](#server) · [리버스 프록시 구현](#reverse-proxy) · [품질](#quality)

<a name="overview"></a>

## 🧭 개요

| 실행 파일 | 역할 |
| --- | --- |
| `webserver` | 정적 파일, 바이너리 `POST /echo`, 선택적 CGI를 제공합니다. |
| `reverse-proxy` | 요청 경로로 백엔드를 선택하고, 요청 전 접속 실패 시 대체 백엔드를 시도합니다. 클라이언트와 백엔드 연결을 각각 재사용합니다. |

두 실행 파일은 HTTP 파서와 이벤트 루프를 공유하지만 독립적으로 실행합니다.

```text
클라이언트 ── HTTP ── reverse-proxy ── HTTP ── webserver A/B
                                                ├─ 정적 파일
                                                ├─ POST /echo
                                                └─ /cgi-bin/이름 → CGI 작업 큐 → 자식 프로세스
```

프록시 없이 `webserver`에 직접 접속할 수도 있습니다.

<a name="requirements"></a>

### 📋 요구 사항

| 단계      | 요구 사항과 구현                                                          |
| ------- | ------------------------------------------------------------------ |
| [Phase 1](docs/PHASE_1_REQUIREMENTS.md) | TCP 리스너, HTTP/1.1 요청 파싱과 응답 생성, 정적 `GET`/`HEAD`, 바이너리 `POST /echo` |
| [Phase 2](docs/PHASE_2_REQUIREMENTS.md) | 논블로킹 I/O, 연결 재사용, 순차 파이프라이닝, `SIGINT`/`SIGTERM` 정상 종료 |
| [Phase 3](docs/PHASE_3_ARCHITECTURE.md) | 경로 기반 중계, 백엔드 풀, 실패와 타임아웃을 처리하는 리버스 프록시 |

단계별 구현 범위와 별도로 [과제 전체 요구사항](docs/REQUIREMENTS.md)을 확인할 수 있습니다.

<a name="development"></a>

### 🐳 개발 환경

빌드·실행·검증의 기준 환경은 **Linux/amd64 Docker**입니다. 개발 이미지에는 GCC, Make, Python 3, curl, Valgrind가 들어 있습니다. WSL에서는 `dev.sh`가 `docker.exe`를 사용합니다.

#### 이미지 빌드 및 개발 셸 시작

```sh
sh dev.sh build
sh dev.sh shell
```

#### 웹 서버 단독 실행

개발 셸에서 `make`로 두 실행 파일을 빌드하거나, `make webserver`와 `make reverse-proxy`로 각각 빌드합니다. `webserver` 단독 실행에는 `--port`와 `--root`가 필요합니다. 아래 예제에서는 공개용 `public/`을 정적 루트로 사용합니다.

```sh
make webserver
./webserver --port 8080 --root public
```

실행 결과는 같은 컨테이너의 다른 셸에서 확인합니다. 호스트에서 `docker ps`로 컨테이너를 찾은 뒤 `docker exec -it <컨테이너-ID> bash`를 실행합니다. WSL에서는 `docker` 대신 `docker.exe`를 사용합니다. `sh dev.sh shell`을 다시 실행하면 새 컨테이너가 시작됩니다.

```sh
curl -i http://127.0.0.1:8080/
printf 'hello' | curl -i --data-binary @- http://127.0.0.1:8080/echo
```

첫 요청은 `public/`의 시작 페이지를 확인하고, 두 번째 요청은 전송한 `hello`가 응답 본문에 그대로 돌아오는지 확인합니다.

<a name="server"></a>

### 🖥️ 서버 구현

#### 1. TCP 소켓 서버

**리스너 시작.** `socket()`으로 IPv4 TCP 논블로킹 소켓을 생성하고, `setsockopt()`로 `SO_REUSEADDR`를 설정합니다. `bind()`로 주소와 포트를 지정한 뒤 `listen(fd, SOMAXCONN)`으로 수락 대기열을 엽니다.

**연결 수락.** 리스너의 `EPOLLIN`은 HTTP 요청이 도착했다는 뜻이 아니라 **수락할 TCP 연결이 준비됐다는 뜻**입니다. `accept4()`는 `SOCK_NONBLOCK | SOCK_CLOEXEC`를 적용한 클라이언트 FD를 반환합니다. 서버는 이 FD에 `client_session`을 만들고 `epoll`에 등록합니다. 이후 `recv()`와 `send()`는 리스너가 아니라 이 FD에서 수행합니다.

리스너는 다음 연결을 받기 위해 유지합니다. 이벤트 한 번에 최대 32개를 수락하며, `accept4()`가 `EAGAIN`을 반환하면 해당 수락 처리를 마칩니다.

#### 2. HTTP 요청 파싱

TCP에서 한 번 읽은 데이터가 HTTP 요청 한 건과 일치하지는 않습니다.
[`cs_process_head()`](src/server/client_session.c)는 연결별 버퍼에서
요청 줄을 파싱하고, 헤더를 읽은 뒤 요청 대상과 헤더 규칙을 검증합니다. `HTTP_PARSE_NEED_MORE`이면 다음 읽기까지
기다리고, 잘못된 입력이면 해당 HTTP 오류 응답을 준비합니다.
헤더 검증 후 [`http_body_init()`과 `http_parse_body()`](src/http/http.c)가
`Content-Length` 또는 chunked 프레이밍에 맞춰 본문을 읽습니다.

```c
/* 요청 줄 파싱과 헤더 검증 — 중간 헤더 파싱·오류 분기는 생략 */
c->line = http_parse_request_line(c->head_buf, c->head_len);
/* ... */
int err = http_validate_request_head(&c->line.request, &c->headers.headers,
                                     &c->target);
```

#### 3. HTTP 응답 생성

앱은 상태 코드, 콘텐츠 타입, 본문 길이, 본문 데이터 또는 파일 FD를 `http_response`에 담습니다.
[`http_format_response_head()`](src/http/http.c)가 상태 줄과 공통 헤더를 직렬화하면,
[`cs_write_response()`](src/server/client_session.c)가 헤더와 본문을 차례로 전송합니다.
한 번에 다 쓰지 못하면 전송 위치를 저장해 이어 보내고, `HEAD` 요청에는 본문을 보내지 않습니다.

```c
/* cs_write_response()에서 헤더 전송 완료 후 */
if (c->head)
    return CS_STATUS_DONE;
```

#### 4. 정적 파일 서빙

[`static_response()`](src/static/static_response.c)는 라우터가 디코딩한 URL 경로를
정적 루트 기준의 상대 경로로 바꿉니다. 디렉터리에서는 `index.html`을 찾습니다. 제공할 인덱스가 있지만
URL에 끝 슬래시가 없으면 슬래시가 있는 URL로 `308` 리디렉션합니다. 허용된 확장자의 일반 파일이면
파일 FD·MIME·크기를 응답에 넘깁니다. 세션은 파일 전체를 메모리에 적재하지 않고 나눠 전송합니다.
[`static_file_open()`](src/static/static_file.c)은 루트 기준 FD로 탐색하며, 실제 열기에는
`openat2` 제약을 적용해 루트 밖이나 다른 마운트로 넘어가지 않도록 합니다.

```c
/* static_response()의 성공 분기 */
response.file_fd = fd;
response.content_type = mime;
response.body.length = (size_t)info.st_size;
```

#### 5. 동적 요청 처리

[`app.c`](src/app/app.c)의 라우트 테이블은 HTTP 메서드와 경로를 C 함수에 연결합니다.
`POST /echo`는 `handle_echo()`를 선택하고, 받은 본문을 **바이트 길이를 포함한 그대로**
`application/octet-stream`으로 반환합니다. 널 바이트를 문자열 종료로 취급하지 않습니다.
등록된 동적 경로에 다른 메서드가 들어오면 [`router.c`](src/app/router.c)가 `405`와 `Allow`를 반환합니다.

```c
static const struct route routes[] = {
    {"POST", "/echo", handle_echo},
};
```

CGI는 기본적으로 비활성화되어 있으며, `--cgi-root`를 지정할 때만 `/cgi-bin/이름`으로 실행합니다.
[CGI 실행기](src/app/cgi.c)는 워커 2개와 대기열 8칸을 사용합니다. 워커가 자식 프로세스에서 CGI를 실행하고,
이벤트 루프는 완료 알림을 받은 뒤 HTTP 응답을 전송합니다. 대기열이 가득 차면 `503`으로 응답합니다.
실행 한도인 5초를 넘으면 자식 프로세스를 정리하고 `504`로 응답합니다. 출력은 1 MiB로 제한합니다.

> CGI는 격리 샌드박스가 아닙니다. **공개 정적 루트나 업로드 경로와 분리하고, 신뢰하는 프로그램만 CGI 루트에 둡니다.**

```sh
demo_cgi=$(mktemp -d)
cc -std=c17 examples/cgi/echo.c -o "$demo_cgi/echo"
./webserver --port 8003 --root public --cgi-root "$demo_cgi"
# 별도 셸:
printf 'hello' | curl -i --data-binary @- http://127.0.0.1:8003/cgi-bin/echo
```

#### 6. 동시 요청 처리

[`epoll_loop_run()`](src/server/epoll_loop.c)은 단일 스레드에서 LT(Level-Triggered) `epoll`로
준비된 연결을 처리합니다. 요청 수신이나 응답 전송이 끝나지 않으면 세션에 진행 상태를 남기고
다른 연결을 처리합니다. 프록시의 백엔드 소켓도 같은 이벤트 루프에서 별도로 감시합니다.
`webserver`는 연결을 재사용하며 파이프라인 요청에도 수신 순서대로 응답합니다.
종료 신호를 받으면 새 연결 수락을 멈추고 유예 시간 동안 진행 중인 요청을 처리합니다.

```c
/* dispatch_events(): 이벤트가 온 소켓에 맞는 세션 경로를 선택 */
bool alive = endpoint->upstream
    ? client_session_advance_upstream(client->session, readable, writable)
    : client_session_advance(client->session, readable, writable);
```

[LT/ET 동작 비교 실습](examples/README.md)에서 두 방식의 차이를 확인할 수 있습니다.

<a name="reverse-proxy"></a>

### 🔀 리버스 프록시 구현

```text
클라이언트 → reverse-proxy → 설정된 HTTP 백엔드
클라이언트 ← reverse-proxy ← 백엔드 응답
```

#### 라우팅과 중계 흐름

1. **백엔드 선택** — [`proxy_routes.c`](src/proxy/proxy_routes.c)는 쿼리를 제외한 경로에서
   가장 긴 접두사 규칙을 찾습니다. 같은 접두사에 등록된 백엔드는 등록 순서대로 번갈아 선택합니다.
   일치하는 규칙이 없으면 `404`를 반환합니다. 클라이언트의 `Host`나 절대 URI로 접속 목적지를 정하지 않습니다.
2. **요청 재구성** — [`proxy_wire.c`](src/proxy/proxy_wire.c)는 경로와 쿼리를 보존하고, `Host`를
   백엔드 주소에 맞춥니다. 연결 단위 헤더를 제거하고 디코딩한 본문의 길이로 `Content-Length`를 설정합니다.
3. **논블로킹 중계** — [`proxy_exchange.c`](src/proxy/proxy_exchange.c)는 백엔드 접속, 요청 쓰기,
   응답 헤더 읽기, 본문 전달을 차례로 처리합니다. 클라이언트에 아직 보내지 못한 데이터가 쌓이면 백엔드 읽기를 잠시 멈춥니다.
   고정 길이 응답은 길이를 유지하고, chunked 응답과 연결 종료(EOF)로 끝나는 응답은 클라이언트에 chunked로 보냅니다.
4. **실패 처리** — 응답을 시작하기 전 접속 실패나 잘못된 백엔드 응답에는 `502`, 기한 초과에는 `504`를 보냅니다.
   요청 쓰기 전 TCP 접속에 실패하면 같은 접두사의 다음 백엔드를 시도하며, 모든 접속 시도는 하나의 2초 기한을 공유합니다.
   쓰기 단계에 들어간 요청은 다시 보내지 않습니다. 클라이언트에 이미 응답을 쓰기 시작했다면 중계 실패 시 연결을 닫습니다.
5. **연결 재사용** — 정상 고정 길이 응답을 끝까지 전송하면 클라이언트 세션은 다음 요청을 받을 수 있습니다.
   백엔드 쪽은 HTTP/1.1 고정 길이 응답을 모두 읽었고 종료 신호나 남은 데이터가 없을 때만
   라우트의 백엔드 항목마다 유휴 소켓을 최대 1개, 최대 5초 동안 보관합니다. chunked·EOF 응답은
   중계하지만 해당 백엔드 연결은 재사용하지 않습니다.

```c
/* 프록시 finish(): 요청 본문 처리 후 백엔드를 선택하고 교환을 생성 */
const struct proxy_route *route = proxy_route_select(proxy->routes, &request->target);
/* 일치하지 않는 경우의 처리 생략 */
struct session_stream *stream =
    proxy_exchange_create(proxy->routes, route - proxy->routes->entries, request, head);
```

#### 백엔드 하나로 실행

개발 컨테이너에서 `make`로 두 실행 파일을 빌드한 뒤, 아래 명령을 같은 컨테이너의 서로 다른 셸에서 실행합니다.
앞의 단독 예제에서 8080 포트를 사용 중이면 해당 서버를 먼저 종료합니다.

```sh
# 터미널 1: 백엔드
./webserver --port 8001 --root public
# 터미널 2: 프록시
./reverse-proxy --port 8080 --route /=127.0.0.1:8001
# 터미널 3: 클라이언트
curl -i http://127.0.0.1:8080/
printf 'hello' | curl -i --data-binary @- http://127.0.0.1:8080/echo
```

프록시에는 `--root`가 필요하지 않으며 백엔드가 아직 실행되지 않아도 시작할 수 있습니다. 백엔드가 다시 올라오면 다음 요청에서 접속을 시도합니다.
실행 중인 8002 백엔드에 대한 규칙 `--route /api/admin/=127.0.0.1:8002`를 추가하면
`/api/admin/status?detail=1`은 더 긴 접두사와 일치하는 8002로 전달합니다. 이 프로젝트의 `webserver`가 아닌 평문 HTTP 서버도
숫자 IPv4 주소로 연결할 수 있습니다. 자세한 정책은 [Phase 3 설계](docs/PHASE_3_ARCHITECTURE.md)에서 확인할 수 있습니다.

#### A/B 라운드 로빈과 장애 전환 데모

브라우저에서 정적 파일과 MIME 응답을 확인하려면 [브라우저 라우팅 실습](docs/ROUTING_DEMO.md)을 참고합니다. 아래는 브라우저 없이 프록시를 확인하는 예제입니다.

8001·8080 포트에서 앞선 예제가 실행 중이면 먼저 종료합니다. 아래 세 명령을 같은 컨테이너의 별도 셸에서 실행합니다.

```sh
./webserver --port 8001 --root examples/round-robin/a
./webserver --port 8002 --root examples/round-robin/b
./reverse-proxy --port 8080 --route /=127.0.0.1:8001 --route /=127.0.0.1:8002
```

`curl -s http://127.0.0.1:8080/`을 반복하면 `Backend A → Backend B → Backend A` 순서로 응답합니다.
A를 종료한 뒤 다시 요청하면 프록시는 B로 전달합니다.

<a name="quality"></a>

### 🧪 품질

#### 단위 테스트

개발 셸에서 `make test-unit test-proxy-unit`을 실행합니다. C 단위 테스트 실행 파일 8개로 HTTP 요청·본문 파서,
세션 앱, 라우터, 정적 파일 처리, 연결 I/O, 프록시 라우팅과 헤더 재구성을 검증합니다. WSL2/GCC 15에서 8개 모두 통과했습니다.

Mull 보조 검사에서는 단위 스위트 5개를 사용했습니다. 변이 검출 / 생존 / 시간 초과 건수는
Phase 1 `353 / 74 / 4`, Phase 2 `86 / 88 / 6`, Phase 3 `221 / 71 / 2`입니다.
이 수치는 전체 코드 커버리지나 과제 합격률을 뜻하지 않습니다.

#### 기능 테스트

```sh
make -B test test-shutdown test-epoll test-keepalive test-proxy test-cgi
```

실제 소켓으로 정적 `GET`/`HEAD`, 바이너리 echo, 타임아웃, 정상 종료, 동시 연결, 연결 재사용과 파이프라이닝을 검증합니다.
프록시는 별도 프로세스로 띄워 양쪽 연결을 독립적으로 재사용하는지, 접속 실패 시 대체 백엔드를 선택하는지,
이미 전송한 `POST`를 다시 보내지 않는지 확인합니다. CGI는 바이너리 요청 처리, CGI 실행 중 정적 응답 처리, 실행 마감과 자식 프로세스 회수를 검증합니다.

#### 부하 테스트

**통합본 공통 측정.** Linux/amd64 Docker, GCC 14.2.0 `-O2`, CPU 2개, 메모리 1 GiB 환경에서
클라이언트 요청마다 새 연결을 열었습니다. 1 KiB 응답을 대상으로 동시성 1·8·32마다
워밍업 100건 뒤 2,000건씩 3회 측정했습니다. 아래 표는 회차별 처리량과 p95 지연 시간의 중앙값입니다.
직접·프록시 합계 36,000건과 기준 백엔드 직접 요청 18,000건은 모두 성공했습니다.

| 경로 | 동시성 1 req/s · p95 | 동시성 8 req/s · p95 | 동시성 32 req/s · p95 |
| --- | ---: | ---: | ---: |
| 직접 정적 GET | 2,443 · 0.66 ms | 1,436 · 10.43 ms | 1,287 · 42.22 ms |
| 프록시 경유 GET | 1,703 · 0.88 ms | 809 · 17.13 ms | 737 · 92.82 ms |
| 기준 백엔드 직접 GET | 903 · 1.78 ms | 639 · 16.18 ms | 652 · 56.77 ms |

공용 Python 백엔드와 부하 생성기를 같은 컨테이너에서 실행했습니다. 이 결과는 운영 처리 한도나
클라이언트 쪽 keep-alive의 성능 개선 폭을 나타내지 않습니다.

**통합 전 Phase 3 참고 측정.** WSL2 루프백에서 Python 벤치마크로 직접 요청과 프록시 경유를 비교했습니다.
아래 표는 GCC `-O2`, 요청마다 새 연결, 동시 클라이언트 32개 조건에서 4회 반복한 **처리량과 회차별 p95의 중앙값**입니다.

| 요청 | 직접 req/s | 직접 p95 | 프록시 경유 req/s | 프록시 경유 p95 |
| --- | ---: | ---: | ---: | ---: |
| 정적 GET 1 KiB | 1,149 | 40.35 ms | 1,104 | 42.30 ms |
| `POST /echo` 64 KiB | 1,128 | 39.51 ms | 870 | 55.33 ms |

동시 클라이언트 1·8·32개를 포함한 비교 전체에서 32,800건이 성공했고,
오류는 0건이었습니다. 측정 종료 후 두 프로세스의 파일 디스크립터 수는 시작 전과 같았습니다.
이 결과도 해당 로컬 환경에서의 비교에만 사용합니다. Phase 1 기준선은 `make benchmark`로 확인할 수 있고,
Phase 2의 연결 재사용 비교는 [Phase 2 검증](docs/PHASE_2_TESTING.md)에 정리했습니다.

#### 메모리 누수 검사

호스트에서 아래 명령으로 개발 환경을 확인하고 Phase 1–3 전체 검증을 실행합니다.
스크립트는 일반 빌드와 ASan/UBSan/LSan 빌드에서 기능 테스트를 실행하고, Valgrind로 C 단위 테스트와 FD를 검사합니다.

```sh
sh dev.sh build
sh dev.sh check
sh tests/verify_phase3.sh
```

Phase 3 CI도 동일한 검증 스크립트를 실행하도록 구성했습니다. [공통 판정 기준](docs/TEAM_COMMON_TEST_PLAN.md)과
[재실행 가이드](docs/TEAM_COMMON_TEST_RUNBOOK.md)에서 실행 조건과 결과 기록 방법을 확인할 수 있습니다.

**로컬 검증 결과.** 통합본은 Linux/amd64 Docker에서 일반·ASan/UBSan/LSan 회귀 테스트와
Valgrind 단위 테스트 8개를 통과했습니다(종료 코드 `0`). 이 기록은 GitHub 호스팅 CI 결과와 별개입니다.

- 별도로 실행한 Valgrind 검사에서 웹 서버와 프록시의 오류는 0건이었습니다. 웹 서버의 FD는 8개로 유지됐고, 프록시의 FD는 7개에서 유휴 연결을 보관한 뒤 8개로 유지됐습니다.
- 일반·sanitizer 회귀 테스트에서 CGI 실행·마감·자식 프로세스 회수를 확인했습니다. CGI를 켠 웹 서버를 별도로 Valgrind에서 실행했을 때도 오류는 0건이었으며, 종료 시 표준 FD 3개만 남았습니다. 외부 CGI 실행 파일 자체는 계측하지 않았습니다.

#### 고려한 엣지 케이스

| 상황 | 처리 |
| --- | --- |
| 요청 줄·헤더·본문이 여러 번에 나뉘어 도착 | 연결별 버퍼와 파서 상태를 유지하고 이어 읽습니다. |
| 본문 길이 헤더 충돌 또는 요청 크기 제한 초과 | 잘못된 프레이밍을 거부하고 요청 줄·헤더·본문에 각각 크기 제한을 적용합니다. |
| 느린 클라이언트 송수신 | 읽기·쓰기 기한과 부분 전송 상태를 관리하면서 다른 연결도 처리합니다. |
| 정적 루트 밖 경로 또는 허용되지 않은 파일 형식 | 루트 경계를 지키고 허용된 MIME 타입의 일반 파일만 제공합니다. |
| `HEAD` 요청 또는 끝 슬래시가 없는 디렉터리 URL | `HEAD`에는 본문을 보내지 않습니다. 인덱스를 제공할 수 있는 디렉터리는 슬래시가 붙은 URL로 `308` 리디렉션합니다. |
| 백엔드 지연 또는 연결 끊김 | 응답 시작 전에는 실패 유형에 따라 `502` 또는 `504`를 보내고, 시작 후에는 새 응답을 보내지 않고 연결을 닫습니다. |
| 종료 신호 수신 또는 연결 한도 도달 | 새 연결 수락을 제어하며, 종료 유예 시간이 지나면 남은 연결과 FD를 정리합니다. |
