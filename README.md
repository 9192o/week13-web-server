# week13-web-server

## 🧭 개요

> 크래프톤 정글 13주차 학습 프로젝트입니다.

- 서버가 TCP 연결을 받는 순간부터 HTTP 응답을 쓰기까지 직접 구현했습니다.
- `webserver`는 **정적 파일, 바이너리 `/echo`, 선택적 CGI**를 제공하고, `reverse-proxy`는 **경로별 백엔드 선택·연결 재시도·양쪽 연결 재사용**을 담당합니다. 두 실행 파일은 HTTP 파서와 이벤트 루프를 공유하지만 독립적으로 실행합니다.

```text
클라이언트 ── HTTP ── reverse-proxy ── HTTP ── webserver A/B
                                                ├─ 정적 파일
                                                ├─ POST /echo
                                                └─ /cgi-bin/이름 → CGI 작업 큐 → 자식 프로세스
```

프록시 없이 `webserver`에 직접 접속할 수도 있습니다. 양쪽 HTTP 연결의 재사용 여부는 각각 결정합니다.

### 📋 요구 사항

| 단계      | 요구 사항과 구현                                                          |
| ------- | ------------------------------------------------------------------ |
| Phase 1 | TCP 리스너, HTTP/1.1 요청 파싱과 응답 생성, 정적 `GET`/`HEAD`, 바이너리 `POST /echo` |
| Phase 2 | 논블로킹 I/O로 여러 연결 처리, 연결 재사용, 순차 파이프라이닝, `SIGINT`/`SIGTERM` 정상 종료    |
| Phase 3 | **Reverse Proxy** 구현: 경로 기반 중계, 백엔드 풀, 실패/타임아웃 처리                  |

### 🐳 개발 환경

- **Linux/amd64 Docker** 환경에서 개발했습니다.
- 개발 이미지에는 GCC, clang/libFuzzer, Make, Python 3, curl, Valgrind가 들어있습니다.
- WSL에서는 `dev.sh`가 `docker.exe`를 사용하여 작동합니다.

```sh
sh dev.sh build
sh dev.sh shell
```

- 개발 셸에서 `make`로 두 실행 파일을 빌드하거나, `make webserver`와 `make reverse-proxy`로 각각 빌드합니다.
- `webserver` 단독 실행에는 `--port`와 `--root`가 필요합니다. **공개용 `public/`을 루트로 사용합니다**.

```sh
make webserver
./webserver --port 8080 --root public
```

- 호스트에서 `docker ps`로 컨테이너를 찾고 `docker exec -it <컨테이너-ID> bash`로 같은 컨테이너에 추가 셸을 엽니다(WSL에서는 `docker.exe`).
- `sh dev.sh shell`을 다시 호출할 시 다른 컨테이너가 시작됩니다.

```sh
curl -i http://127.0.0.1:8080/
printf 'hello' | curl -i --data-binary @- http://127.0.0.1:8080/echo
```

### 🖥️ 서버 구현

#### 1. TCP 소켓 서버

**서버 시작 요약**
```c
1. socket(..., SOCK_STREAM | SOCK_NONBLOCK, ...) ->
2. setsockopt(..., SO_REUSEADDR, ...) ->
3. bind() ->
4. listen(fd, SOMAXCONN) ->
```
- 서버 시작 시
	1. `IPv4 TCP`, 논블로킹 소켓을 생성합니다.
	2. `SO_REUSEADDR` 설정으로, 서버 재시작 시 같은 주소와 포트에 다시 바인딩하기 쉽게 설정합니다.
	3. 소켓에 `127.0.0.1:8080`을 바인딩합니다.
	4. 소켓을 연결 수락용으로 전환합니다.
- 이후 커널이 수락 대기중인 연결을 관리하고, 서버는 `accept4()`로 하나씩 꺼냅니다.

**연결 수락 요약**
```c
1. epoll_wait() -> EPOLLIN ->
2. accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC) ->
3. client_session 생성 → epoll에 연결 FD 등록
```

- 클라이언트가 연결하면
	1. `epoll_wait()`가 리스너의 `EPOLLIN` 이벤트를 반환합니다. 이 이벤트는 HTTP 요청이 도착했다는 뜻이 아니라, **수락할 연결이 준비됐다는 뜻**입니다.
	2. `accept4()`로 대기 중인 연결을 꺼내고, 연결 전용 클라이언트 FD를 얻습니다.
	3. 클라이언트 FD마다 `client_session`을 만들어 요청 버퍼와 송수신 상태를 관리합니다.
	4. 클라이언트 FD를 `epoll`에 등록해 읽기 이벤트를 기다립니다. 이후 `recv()`와 `send()`는 이 FD에서 수행합니다.
- 리스너 FD는 그대로 유지해 다음 연결을 받습니다. 한 번에 최대 32개를 수락하며, `accept4()`가`EAGAIN`을 반환하면 이번 수락을 마칩니다.
#### 2. HTTP 요청 파싱

TCP에서 한 번 읽은 데이터가 HTTP 요청 한 건과 일치하지는 않는다.
[`cs_process_head()`](src/server/client_session.c)는 연결별 버퍼에서
요청 줄 → 헤더 → 요청 대상·헤더 규칙 순으로 처리한다. `HTTP_PARSE_NEED_MORE`이면 다음 읽기까지
기다리고, 잘못된 입력이면 해당 HTTP 오류 상태를 응답으로 예약한다.
헤더 검증 후 [`http_body_init()`과 `http_parse_body()`](src/http/http.c)가
`Content-Length` 또는 chunked 프레이밍에 맞춰 본문을 읽는다.

```c
/* 요청 줄 파싱과 헤더 검증 — 중간 헤더 파싱·오류 분기는 생략 */
c->line = http_parse_request_line(c->head_buf, c->head_len);
/* ... */
int err = http_validate_request_head(&c->line.request, &c->headers.headers,
                                     &c->target);
```

#### 3. HTTP 응답 생성

앱은 상태 코드, 콘텐츠 타입, 본문 길이, 본문 데이터 또는 파일 FD를 `http_response`에 담는다.
[`http_format_response_head()`](src/http/http.c)가 상태 줄과 공통 헤더를 바이트로 만들고,
[`cs_write_response()`](src/server/client_session.c)는 헤더부터 보낸 뒤 본문을 전송한다.
한 번에 다 쓰지 못하면 전송 위치를 저장해 이어 보내고, `HEAD`는 헤더를 보낸 뒤 끝낸다.

```c
/* cs_write_response()에서 헤더 전송 완료 후 */
if (c->head)
    return CS_STATUS_DONE;
```

#### 4. 정적 파일 서빙

[`static_response()`](src/static/static_response.c)는 라우터가 디코딩한 URL 경로를
정적 루트 기준의 상대 경로로 바꾼다. 디렉터리는 `index.html`을 찾고, 제공할 인덱스가 있는
디렉터리 URL에 끝 슬래시가 없으면 `308`로 안내한다. 공개 확장자와 일반 파일 여부를 확인한 뒤
파일 FD·MIME·크기를 응답에 넘긴다. 세션은 파일 전체를 메모리에 적재하지 않고 나눠 전송한다.
[`static_file_open()`](src/static/static_file.c)은 루트 기준 FD로 탐색하며, 실제 열기에는
`openat2` 제약을 걸어 루트 밖이나 다른 마운트로 넘어가지 않게 한다.

```c
/* static_response()의 성공 분기 */
response.file_fd = fd;
response.content_type = mime;
response.body.length = (size_t)info.st_size;
```

#### 5. 동적 요청 처리

[`app.c`](src/app/app.c)의 라우트 테이블은 HTTP 메서드와 경로를 C 함수에 연결한다.
`POST /echo`는 `handle_echo()`를 선택하고, 받은 본문을 **바이트 길이를 포함한 그대로**
`application/octet-stream`으로 반환한다. 널 바이트가 섞여도 문자열로 잘라내지 않는다.
등록된 동적 경로에 다른 메서드가 들어오면 [`router.c`](src/app/router.c)가 `405`와 `Allow`를 반환한다.

```c
static const struct route routes[] = {
    {"POST", "/echo", handle_echo},
};
```

CGI는 기본 비활성이며, 운영자가 신뢰하는 실행 파일 디렉터리를 `--cgi-root`로 지정할 때만
`/cgi-bin/이름`에서 실행한다. [CGI 실행기](src/app/cgi.c)는 워커 2개와 대기열 8칸을 둔다.
워커는 자식 프로세스에서 CGI를 실행하고, 이벤트 루프가 완료 알림을 받은 뒤 HTTP 응답을 쓴다.
대기열이 가득 차면 `503`, 실행 한도 5초를 넘으면 자식을 정리하고 `504`로 응답한다.
출력은 1 MiB로 제한한다. CGI는 격리 샌드박스가 아니다. **업로드 경로나 공개 정적 루트를
CGI 루트로 지정하지 말고 신뢰하는 프로그램만 두어야 한다.**

```sh
demo_cgi=$(mktemp -d)
cc -std=c17 examples/cgi/echo.c -o "$demo_cgi/echo"
./webserver --port 8003 --root public --cgi-root "$demo_cgi"
# 별도 셸:
printf 'hello' | curl -i --data-binary @- http://127.0.0.1:8003/cgi-bin/echo
```

#### 6. 동시 요청 처리

[`epoll_loop_run()`](src/server/epoll_loop.c)은 단일 스레드에서 LT(Level-Triggered) `epoll`로
준비된 연결을 처리한다. 요청을 덜 받았거나 응답을 다 못 보냈으면 세션에 진행 상태를 남기고
다른 연결로 넘어간다. 프록시의 백엔드 소켓도 같은 이벤트 루프에서 별도로 감시한다.
`webserver`는 연결을 재사용하며 파이프라인 요청도 수신 순서대로 응답한다.
종료 신호를 받으면 새 연결 수락을 멈추고 진행 중인 요청을 유예 시간 안에서 마무리한다.

```c
/* dispatch_events(): 이벤트가 온 소켓에 맞는 세션 경로를 선택 */
bool alive = endpoint->upstream
    ? client_session_advance_upstream(client->session, readable, writable)
    : client_session_advance(client->session, readable, writable);
```

[LT/ET 동작을 비교하는 별도 실습](examples/README.md)도 있다.

### 🔀 Proxy/Reverse Proxy 구현

```text
클라이언트 → reverse-proxy → 설정된 HTTP 백엔드
클라이언트 ← reverse-proxy ← 백엔드 응답
```

1. **백엔드 선택** — [`proxy_routes.c`](src/proxy/proxy_routes.c)가 쿼리를 제외한 경로에서
   가장 긴 접두사를 찾는다. 같은 접두사에 등록된 백엔드만 등록 순서대로 라운드 로빈한다.
   일치 규칙이 없으면 `404`다. 클라이언트의 `Host`나 절대 URI로 접속 목적지를 정하지 않는다.
2. **요청 재구성** — [`proxy_wire.c`](src/proxy/proxy_wire.c)가 경로·쿼리를 보존하고, `Host`는
   백엔드 주소에 맞춘다. 연결 단위 헤더를 제거하고 디코딩한 본문의 길이로 `Content-Length`를 설정한다.
3. **논블로킹 중계** — [`proxy_exchange.c`](src/proxy/proxy_exchange.c)가 접속 → 요청 쓰기 →
   응답 헤더 읽기 → 본문 전달을 처리한다. 클라이언트에 보낼 데이터가 밀리면 백엔드 읽기를 잠시 멈춘다.
   고정 길이 응답은 길이를 유지하고, chunked·EOF 구분 응답은 클라이언트 쪽에서 chunked로 보낸다.
4. **실패 처리** — 응답을 시작하기 전 접속 실패·잘못된 백엔드 응답에는 `502`, 기한 초과에는 `504`를 보낸다.
   아직 요청을 쓰기 전 TCP 접속이 실패하면 같은 접두사의 다음 백엔드를 시도한다. 접속 예산 2초는 시도 전체가 공유한다.
   쓰기 단계에 들어간 요청은 다시 보내지 않는다. 클라이언트에 응답을 이미 쓰기 시작했다면 중계 실패 시 연결을 닫는다.
5. **연결 재사용** — 정상 고정 길이 응답을 끝까지 전송하면 클라이언트 세션은 다음 요청을 받을 수 있다.
   백엔드 쪽은 HTTP/1.1 고정 길이 응답을 모두 읽었고 종료 신호나 남은 데이터가 없을 때만
   라우트의 백엔드 항목별 유휴 소켓을 1개, 5초까지 보관한다. chunked·EOF 응답은
   여전히 중계하지만 재사용하지 않는다.

```c
/* 프록시 finish(): 요청 본문 처리 후 백엔드를 선택하고 교환을 생성 */
const struct proxy_route *route = proxy_route_select(proxy->routes, &request->target);
/* 일치하지 않는 경우의 처리 생략 */
struct session_stream *stream =
    proxy_exchange_create(proxy->routes, route - proxy->routes->entries, request, head);
```

**실행 예시.** 개발 컨테이너에서 `make`로 두 실행 파일을 빌드한 뒤, 아래 명령을 서로 다른 셸에서 실행한다.
앞의 단독 예제에서 8080을 사용 중이면 먼저 종료한다.

```sh
# 터미널 1: 백엔드
./webserver --port 8001 --root public
# 터미널 2: 프록시
./reverse-proxy --port 8080 --route /=127.0.0.1:8001
# 터미널 3: 클라이언트
curl -i http://127.0.0.1:8080/
printf 'hello' | curl -i --data-binary @- http://127.0.0.1:8080/echo
```

프록시는 `--root`나 백엔드 기동을 기다리지 않고 시작한다. 백엔드가 다시 올라오면 다음 요청에서
다시 접속한다. 8002의 백엔드에 `--route /api/admin/=127.0.0.1:8002`를 추가하면
`/api/admin/status?detail=1`은 8002로 전달된다. 반면 같은 `/` 접두사로 추가하면
8001 → 8002 → 8001 순서로 선택한다. 이 프로젝트의 `webserver`가 아닌 별도 평문 HTTP 서버도
숫자 IPv4 주소로 연결할 수 있다. 자세한 정책은 [Phase 3 설계](docs/PHASE_3_ARCHITECTURE.md)에 있다.

**A/B 발표 데모:** 아래 세 줄을 별도 셸에서 실행하고 `curl -s http://127.0.0.1:8080/`을
반복한다. `Backend A → Backend B → Backend A` 순으로 보인다. A를 종료하면 이후 요청은 살아 있는
B로 전달된다. 이미 실행 중인 8001·8080 예제는 먼저 종료한다.

```sh
./webserver --port 8001 --root examples/round-robin/a
./webserver --port 8002 --root examples/round-robin/b
./reverse-proxy --port 8080 --route /=127.0.0.1:8001 --route /=127.0.0.1:8002
```

### 🧪 품질

#### 단위 테스트

개발 셸에서 `make test-unit test-proxy-unit`을 실행한다. C 테스트 실행 파일 8개로 HTTP 요청·본문 파서,
세션 앱, 라우터, 정적 파일 처리, 연결 I/O, 프록시 라우팅과 헤더 재구성을 검증한다. WSL2/GCC 15에서 8개가 통과했다.

Mull 보조 검사에서는 단위 스위트 5개로 뮤테이션을 검사했다. 검출 / 생존 / 시간 초과는
Phase 1 `353 / 74 / 4`, Phase 2 `86 / 88 / 6`, Phase 3 `221 / 71 / 2`였다.
전체 코드 커버리지나 과제 합격률은 아니다.

#### 기능 테스트

```sh
make -B test test-shutdown test-epoll test-keepalive test-proxy test-cgi
```

실제 소켓으로 정적 GET/HEAD, 바이너리 echo, 타임아웃, 정상 종료, 동시 연결, 재사용·파이프라인을 검사한다.
프록시는 별도 프로세스로 띄워 양쪽 연결의 독립적 재사용, 접속 장애 시 대체 경로,
전송한 POST의 미재시도를 확인한다. CGI는 바이너리 요청과 느린 자식 처리 중 정적 응답, 마감과 자식 회수를 확인한다.

#### Fuzz 테스트

`tests/fuzz_http.c`는 HTTP 요청/헤더/본문 파서와 프록시 응답 헤더 파서를
libFuzzer로 검사한다. `tests/fuzz_corpus/`의 seed를 시작점으로 사용하며,
생성된 입력은 `fuzz_work/`에만 쓰고 커밋하지 않는다.

```sh
make fuzz FUZZ_SECONDS=600
```

`clang`의 `-fsanitize=fuzzer,address,undefined`를 사용하므로 개발 이미지에
`clang`과 libFuzzer runtime이 포함되어 있다. Fuzz는 네트워크·파일 시스템이 아닌
파서 robustness 확인이며, 전체 서버 동작이나 커버리지를 보증하지 않는다.
실행 결과와 seed 목록은 [Fuzzing](docs/FUZZING.md)에 정리했다.

#### 부하 테스트

`feat/final` 공통 측정은 Linux/amd64 Docker, GCC 14.2.0 `-O2`, CPU 2개,
1 GiB, 클라이언트 요청마다 새 연결에서 실행했다. 1 KiB 응답을 대상으로 동시성
1·8·32마다 워밍업 100건 뒤 2,000건씩 3회 측정한 중앙값이다. 직접·프록시 합계
36,000건과 기준 백엔드 직접 18,000건 모두 성공했다.

| 경로 | 동시성 1 req/s · p95 | 동시성 8 req/s · p95 | 동시성 32 req/s · p95 |
| --- | ---: | ---: | ---: |
| 직접 정적 GET | 2,443 · 0.66 ms | 1,436 · 10.43 ms | 1,287 · 42.22 ms |
| 프록시 경유 GET | 1,703 · 0.88 ms | 809 · 17.13 ms | 737 · 92.82 ms |
| 기준 백엔드 직접 GET | 903 · 1.78 ms | 639 · 16.18 ms | 652 · 56.77 ms |

공용 Python 백엔드와 부하 생성기를 같은 컨테이너에서 실행한 결과다.
운영 처리 한도나 클라이언트 쪽 keep-alive의 성능 개선 폭으로 해석하지 않는다.

아래는 **통합 전 Phase 3 브랜치의 참고 기록**이다. 직접 요청과 프록시 경유를 WSL2 루프백에서 Python 벤치마크로 비교했다. GCC `-O2`, 요청마다 새 연결,
동시 클라이언트 32개 조건에서 4회 반복한 **처리량과 회차별 p95의 중앙값**이다.

| 요청 | 직접 req/s | 직접 p95 | 프록시 경유 req/s | 프록시 경유 p95 |
| --- | ---: | ---: | ---: | ---: |
| 정적 GET 1 KiB | 1,149 | 40.35 ms | 1,104 | 42.30 ms |
| `POST /echo` 64 KiB | 1,128 | 39.51 ms | 870 | 55.33 ms |

동시 클라이언트 1·8·32개를 포함한 비교 전체에서 32,800건이 성공했고,
오류는 0건이었다. 측정 종료 후 두 프로세스의 파일 디스크립터 수는 시작 전과 같았다.
이 값은 로컬 비교용이며 운영 환경의 처리 한도를 뜻하지 않는다. Phase 1 기준선은 `make benchmark`로 실행할 수 있고,
Phase 2의 연결 재사용 비교는 [Phase 2 검증](docs/PHASE_2_TESTING.md)에 있다.

#### 메모리 누수 검사

호스트에서 아래 명령으로 개발 환경을 확인하고 Phase 1–3 전체 검증을 실행한다.
스크립트는 일반 빌드와 ASan/UBSan/LSan 빌드의 기능 스위트, Valgrind의 C 단위 검사 및 FD 추적을 실행한다.

```sh
sh dev.sh build
sh dev.sh check
sh tests/verify_phase3.sh
```

Phase 3 CI도 동일한 검증 스크립트를 실행한다. [공통 판정 기준](docs/TEAM_COMMON_TEST_PLAN.md)과
[재실행 가이드](docs/TEAM_COMMON_TEST_RUNBOOK.md)에서 실행 조건과 결과 기록 방법을 확인할 수 있다.

최종 변경본은 위 스크립트의 일반·ASan/UBSan/LSan 회귀와 Valgrind 단위 8개를
로컬 Linux/amd64 Docker에서 통과했다(종료 0). 별도 공통 실행에서 직접 서버·프록시
프로세스의 Valgrind 오류는 0건이었다. FD는 직접 서버 8개로 유지됐고 프록시는
7개에서 풀에 유휴 연결이 생긴 뒤 8개로 유지됐다. CGI 실행·마감·자식 회수는
일반·sanitizer 회귀에서 확인했다. CGI를 켠 웹서버도 별도 Valgrind 실행에서
오류 0건, 종료 시 표준 FD 3개만 남았다. 외부 CGI 실행 파일 자체는 계측 범위 밖이다.
GitHub 호스팅 CI는 이 로컬 결과와 별개다.

#### 고려한 엣지 케이스

| 상황 | 처리 |
| --- | --- |
| 요청 줄·헤더·본문이 여러 번에 나뉘어 도착 | 연결별 버퍼와 파서 상태를 유지하고 필요한 만큼 이어 읽음 |
| 본문 길이 헤더가 충돌하거나 요청 크기 제한을 초과 | 잘못된 프레이밍을 거부하고 요청 줄·헤더·본문 제한을 각각 적용 |
| 클라이언트가 천천히 보내거나 응답을 천천히 읽음 | 읽기·쓰기 마감시간과 부분 전송 상태를 적용해 다른 연결도 처리 |
| 정적 경로가 루트 밖을 가리키거나 비공개 형식 요청 | 루트 경계를 적용하고 허용 MIME·일반 파일 조건을 만족한 파일만 서빙 |
| `HEAD` 요청 또는 디렉터리 URL에 끝 슬래시 없음 | `HEAD`는 본문 생략, 제공 가능한 인덱스가 있을 때 슬래시 경로로 `308` |
| 백엔드가 느리거나 연결이 끊김 | 응답 시작 전에는 `502`/`504`, 시작 후에는 불완전한 응답을 새 상태 줄 없이 종료 |
| 종료 신호 수신 또는 연결 한도 도달 | 새 연결 수락을 제어하고 종료 유예 이후 남은 연결과 FD를 정리 |
