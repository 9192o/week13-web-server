# week13-web-server

크래프톤 정글 13주차 과제. **C 로 만든 HTTP/1.1 웹서버.**
외부 HTTP 라이브러리 없이 표준 C 라이브러리, POSIX 소켓 API, pthread 만 쓴다.

| Phase | 내용 | 상세 문서 |
| --- | --- | --- |
| 1 | TCP 소켓 서버, 요청 파싱, 정적 파일, POST 본문, CGI | [docs/phase1.md](docs/phase1.md) |
| 2 | 스레드 풀, keep-alive, 느린 클라이언트 방어, 정상 종료 | [docs/phase2.md](docs/phase2.md), [부하 테스트](docs/bench-phase2.md) |
| 3 | 리버스 프록시 (경로 라우팅, 라운드로빈, 502/504) | [docs/phase3.md](docs/phase3.md) |

각 phase 는 유저 시나리오(S1~S11, P2-S1~S5, P1~P8)를 먼저 쓰고, 한 단위씩 "결정 → 구현 → 테스트"로 만들었다.
시나리오가 그대로 기능 테스트가 된다. 설계 결정마다 이유와 검토한 대안은 phase 문서에 있다.

## 전체 구조

```
client ─▶ :8080 server ─┬─ -R 규칙에 맞는 경로 ─▶ 백엔드 A :8001 / B :8002 (라운드로빈)
                        └─ 규칙에 없는 경로 ───▶ 정적 파일 (www/) 또는 CGI (www/cgi-bin/)

메인 스레드                                  워커 스레드 x N (기본 8)
accept → TCP_NODELAY → 큐에 push --> [연결 큐 64칸] --> pop → 요청 처리 (keep-alive 루프)
              └ 큐가 가득 차면 기다리지 않고 503 후 close
```

요청 하나의 흐름:

```
read_request_head   CRLFCRLF 까지 읽기 (16KB, 헤더 전체 5초)
 → http_parse_request   414 → 400 → 501 → 505 → 헤더 검사
 → url_check_path       디코딩 → ".." 거부 → 점 파일 404
 → -R 규칙에 맞음?  예: 백엔드로 중계 (serve_proxy)
 → /cgi-bin 인가?   예: fork+execve 로 CGI 실행 / 아니오: 정적 파일 (GET 만)
 → keep-alive 가능하면 다음 요청 대기, 아니면 close
```

| 모듈 | 역할 |
| --- | --- |
| `src/main.c`, `config.c` | 시작, CLI 옵션, 시그널 |
| `src/server.c` | accept 루프, 요청 읽기, 요청 처리 루프(keep-alive), 프록시 중계, 정상 종료 |
| `src/queue.c`, `pool.c` | 연결 큐(생산자-소비자), 워커 풀 |
| `src/http_parser.c`, `http_response.c` | 요청 파싱(제자리, 엄격 모드), 응답 헤더·에러 페이지·접근 로그 |
| `src/url.c`, `static.c` | 경로 검사(디코딩, `..`, 점 파일, `realpath`), 정적 파일·MIME |
| `src/body.c`, `cgi.c` | POST 본문 읽기, CGI 실행·출력 해석 |
| `src/proxy.c` | 라우팅 규칙, 라운드로빈, 헤더 재작성, 응답 검증 (소켓 없는 순수 로직) |

## 빌드와 실행

```sh
make                            # ./server 와 www/cgi-bin/adder 를 빌드
./server -p 8080 -r ./www       # 기본값: 포트 8080, 루트 ./www
```

| 옵션 | 의미 | 기본 |
| --- | --- | --- |
| `-p` | 포트 (1~65535) | 8080 |
| `-r` | 정적 파일 루트 (디렉터리, `/` 는 거부) | `./www` |
| `-w` | 워커 스레드 수 (1~256) | 8 |
| `-q` | 연결 대기 큐 칸 수 (1~4096) | 64 |
| `-R prefix=host:port[,host:port...]` | 리버스 프록시 규칙 (여러 번 가능, 최대 16개, 규칙마다 백엔드 최대 8개) | 없음 |

- 종료는 `Ctrl+C` 또는 `kill -TERM <pid>`. 처리 중인 요청은 끝까지 보낸다(최대 10초). 종료 코드는 정상 0, 시작 실패 1, 강제 종료 2.
- 시간 제한은 `-D` 로 바꿀 수 있다 (예: `HEADER_TIMEOUT_MS`).
- 개발·테스트 환경은 WSL2 Ubuntu (Linux/macOS 의 POSIX 환경이면 된다).

### 실행 예시

```sh
curl -i localhost:8080/index.html                      # 정적 파일, text/html
curl -i localhost:8080/nope.html                       # 404
curl -i "localhost:8080/cgi-bin/adder?a=1&b=2"         # CGI (GET): 3
curl -i -d 'a=1&b=2' localhost:8080/cgi-bin/adder      # CGI (POST): 3
curl -i -X DELETE localhost:8080/index.html            # 501
```

접근 로그는 stderr 에 요청마다 한 줄이다: `ip:port "요청 첫 줄" 상태 본문바이트 [비고]`.

### 데모 페이지

`www/index.html` 이 정적 파일·MIME, CGI(GET/POST), 리버스 프록시 라운드로빈을 브라우저에서 확인하는 데모다.
개발자도구 Network 탭을 열고 보면 Content-Type, 상태 코드, keep-alive 연결 재사용이 보인다.

```sh
make demo-files                                         # 100MB 더미 파일 www/big.bin (큰 파일 전송 중 끊기·종료 시연용, 커밋 안 함)
./server -p 8001 -r ./www-a                             # 백엔드 두 대 (응답에 자기 포트가 들어 있음)
./server -p 8002 -r ./www-b
./server -p 8080 -r ./www -R /api/=127.0.0.1:8001,127.0.0.1:8002
# 브라우저: http://localhost:8080/   (3번 섹션 버튼 → 8001, 8002 가 번갈아 응답)
```

## 기능 요약

### Phase 1: 순차 HTTP 서버

- **소켓:** `socket → SO_REUSEADDR → bind → listen → accept`. 단계마다 실패하면 이유를 출력.
- **파싱:** GET·POST, HTTP/1.0·1.1. 완성된 헤더 버퍼를 제자리 파싱(malloc 없음). 엄격 모드(CRLF 만, 헤더 64개, Content-Length 숫자 검증, CL+TE 거부).
  오류는 곧바로 상태 코드로: 414/400/501/505/431.
- **정적 파일:** 퍼센트 디코딩 → `..` 거부 → 점 파일 404 → `realpath` 로 루트 밖이면 403 (2중 방어). 디렉터리는 index.html, 없으면 403, 슬래시가 없으면 301.
  MIME 는 확장자 고정 표. 큰 파일은 64KB 씩 스트리밍.
- **POST·CGI:** Content-Length 만큼만 읽고(411/413/408, 100-continue) `fork + execve` 로 실행(셸 없음). 환경 변수는 새로 구성하고,
  stdin 쓰기와 stdout 읽기를 `poll` 로 동시에 해서 교착을 피한다. 출력은 전부 모은 뒤 서버가 헤더를 작성하므로 CGI 가 중간에 죽으면 500.
- **시간 제한:** 헤더 전체 5초(slowloris 방어), 본문 10초, CGI 5초.

### Phase 2: 동시성

- **스레드 풀:** 메인은 accept 만, 워커 N 개가 큐에서 연결을 꺼내 처리. 큐가 가득 차면 기다리지 않고 즉시 503.
  `fork` / 스레드 풀 / `epoll` 을 비교해서 스레드 풀을 골랐다(블로킹 코드 재사용, 스레드 수 상한).
- **keep-alive:** HTTP/1.1 은 기본 유지, 응답 경계가 확실할 때만 유지, idle 5초, 연결당 100개, pipelining(남은 바이트를 다음 요청으로 이어 읽음).
- **정상 종료:** `SIGINT`/`SIGTERM` 은 메인 스레드만 받는다. listen 소켓을 먼저 닫고, 처리 중인 요청은 마무리하고, 10초가 지나거나 두 번째 신호가 오면 강제 종료.
- **동시성 버그 수정:** 공유 정적 버퍼를 지역 배열로, CGI 자식이 다른 워커의 fd 를 물려받는 문제를 exec 직전 fd 일괄 close 로.

| 시간 제한 | 기본 | 넘기면 |
| --- | --- | --- |
| `HEADER_TIMEOUT_MS` | 5초 | 한 바이트도 없었으면 close, 일부라도 왔으면 408 |
| `KEEPALIVE_TIMEOUT_MS` | 5초 | 응답 없이 close |
| `BODY_TIMEOUT_MS` | 10초 | 408 |
| `SEND_TIMEOUT_MS` | 30초 | close |
| `SHUTDOWN_TIMEOUT_MS` | 10초 | 강제 종료 (exit 2) |

### Phase 3: 리버스 프록시

- **라우팅:** 디코딩된 경로의 prefix 매칭, 여러 개면 가장 긴 것. 규칙에 없는 경로는 프록시가 직접 서빙(`-R` 없이 띄우면 phase 2 서버와 같다).
- **로드밸런싱:** 규칙마다 `atomic_fetch_add` 카운터로 라운드로빈 (요청 단위, lost update 없음).
- **백엔드 연결:** `HTTP/1.0` + `Connection: close` 로 요청해서 chunked 응답을 없앴다. 응답에 Content-Length 가 있으면 클라이언트 keep-alive 유지. hop-by-hop 헤더는 양쪽에서 제거.
- **장애 처리:**

| 상황 | 결과 |
| --- | --- |
| connect 거절 | 다음 백엔드로 재시도 (백엔드마다 한 번), 모두 실패하면 502 |
| connect·응답 헤더 5초 초과 | 504 (요청을 이미 보냈으면 재시도 안 함) |
| 깨지거나 애매한 응답 (상태 줄, 1xx, CL 충돌, TE) | 502 (request smuggling 차단) |
| 헤더 전송 뒤 백엔드가 죽음·멈춤 | 클라이언트 연결을 끊음 (curl exit 18) |
| 중계 중 클라이언트가 끊음 | 중계 중단, fd 두 개 모두 정리 |

- **POST 전달:** 본문을 CGI 와 같은 규칙으로 다 받은 뒤 백엔드에 연결한다. 느린 클라이언트가 백엔드 연결을 붙잡지 않고, 재시도도 안전하다.
- **자기 자신을 가리키는 규칙**은 시작할 때 거부 (무한 루프 방지).

## 테스트

```sh
make test         # 단위 테스트 (자체 assert 매크로, tests/test_*.c)
make asan         # ASan + UBSan + LSan 빌드로 단위 테스트
make ftest        # ASan 서버를 띄워 기능 시나리오 전체 (시간 제한을 짧게 줄인 빌드, nc 필요)
make ftest-slow   # 같은 테스트를 실제 시간 제한 그대로 (약 1분)
make valgrind     # valgrind 아래에서 기능 테스트
bench/run.sh      # wrk 부하 테스트 (phase 2)
```

| 종류 | 내용 |
| --- | --- |
| 단위 | 설정, 파서(414 경계, 400 형식, 501/505/431), 응답 생성(헤더 전체를 바이트 단위로 비교), 요청 읽기, URL 검사, 정적 파일, 본문, CGI, 큐, 프록시(라운드로빈 동시 선택, 깨진 응답 20여 가지) |
| 기능 | `tests/functional.sh` (bash + curl + nc). 시나리오 전체: 정적·파싱·본문·CGI, 동시 접속·느린 클라이언트·큐 가득·keep-alive·종료, 프록시 P1~P8 |
| 메모리 | ASan·UBSan·LSan, valgrind. 기능 테스트가 `/proc/<pid>/fd` 개수를 전후로 비교해 fd 누수를 검사 |
| 부하 | `wrk` (130바이트 정적 파일, 동시 연결 10/100/1000, 워커 8/64, keep-alive 유무) |

### 부하 테스트에서 본 것 (phase 2)

| 비교 | 결과 |
| --- | --- |
| 순차(phase 1) → 스레드 풀, keep-alive 없음, 동시 1000 | 24,964 → **69,816 req/s** (2.8배) |
| keep-alive 효과 (워커 64, 동시 100) | 53,701 → **145,420 req/s** (2.7배) |
| 워커 8 + keep-alive 에서 동시 100~1000 | 요청의 약 10% 가 503. 큐를 키우면 503 은 사라지지만 p99 가 1.3초로 늘어남 |

- 503 의 원인은 큐 크기가 아니라 **keep-alive 연결이 워커를 붙잡는 것**이다.
- 측정 덕에 Nagle 알고리즘 + 지연 ACK 로 keep-alive 요청마다 40ms 가 늦어지는 문제를 찾아 `TCP_NODELAY` 로 고쳤다 (동시 10: 184 → 77,626 req/s).

## 엣지 케이스 (일부)

| 케이스 | 결과 |
| --- | --- |
| 헤더 일부만 보내고 대기 / 한 바이트씩 천천히 | 5초 뒤 408 |
| 연결만 열고 아무것도 안 보냄 | 5초 뒤 응답 없이 close, 그동안 다른 클라이언트는 정상 |
| 요청 라인 8192바이트 초과 | 414 (lingering close 덕분에 응답이 전달됨) |
| `DELETE`, `Transfer-Encoding: chunked` | 501 |
| `..`, `%2e%2e`, `%00` | 400 |
| `.env`, 루트 밖을 가리키는 심볼릭 링크 | 404, 403 |
| CGI 크래시 / 5초 초과 / `Status: 204` | 500 |
| 100MB 전송 도중 클라이언트가 끊음 | 서버 생존, 로그 `(aborted)` |
| 워커·큐가 모두 참 | 새 연결은 즉시 503 |
| 백엔드 꺼짐 / 무응답 | 502 / 504 |

## 알려진 한계

- **idle keep-alive 연결이 워커를 붙잡는다.** 워커 수만큼의 클라이언트가 연결만 열어 둬도 새 연결은 큐에서 기다리고, 큐까지 차면 503 이다.
  이벤트 루프(`epoll`)는 쉬는 연결이 fd 하나라 이 문제가 없지만 읽기·쓰기·CGI·connect 를 모두 논블로킹 상태 머신으로 다시 짜야 한다.
- **강제 종료가 CGI·백엔드 대기 중이면 늦어진다.** 소켓을 끊어도 CGI 파이프나 백엔드를 기다리는 워커는 각자의 시간 제한(5초)까지 깨어나지 않는다.
- **503 은 RST 에 질 수 있다.** 요청이 서버의 `close` 뒤에 도착하면 클라이언트가 503 대신 `connection reset` 을 볼 수 있다.
- **프록시:** connect 재시도는 백엔드마다 시간을 따로 재서 최악 (백엔드 수 x 5초) 뒤에 504 가 된다.
  다른 로컬 IP 나 다른 프록시를 거쳐 돌아오는 루프는 막지 못한다. 이 서버는 `0.0.0.0` 으로 listen 하므로 백엔드를 실제로 숨기려면 방화벽이나 바인드 주소가 따로 필요하다.
- **범위 밖:** chunked 전송, HEAD, 조건부·범위 요청, HTTP/1.0 keep-alive, IPv6, 포워드 프록시, 캐싱.
- **WSL 의 `/mnt/d`:** 모든 파일이 실행 가능으로 보여 "실행 권한 없음 → 403" 이 동작하지 않고, 파일 접근이 느려 부하 테스트에 영향을 준다 (`/tmp` 에서 실행 권장).

## 추후 작업

- `epoll` 이벤트 루프로 워커 점유 문제를 풀고, 같은 `bench/run.sh` 로 처리량·지연·메모리를 비교.
- ThreadSanitizer 로 기능 테스트를 돌려 경쟁 조건 검사.
- 프록시 부하 테스트 (백엔드 2개 + 프록시, 라운드로빈 분배 500 : 500 확인), 실패한 백엔드를 건너뛰는 health check, `X-Forwarded-For`, 백엔드 연결 재사용.
