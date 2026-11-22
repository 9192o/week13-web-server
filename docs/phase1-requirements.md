# Phase 1 요구사항 — 동시성 이전 HTTP 서버

> 상위 인덱스: [requirements.md](requirements.md)
> 과제 명세를 source of truth로 삼는다. 이 파일은 Phase 1 범위만 다룬다.

## 0. 범위

- 단일 연결을 순차 처리하는 HTTP/1.1 서버.
- TCP 소켓, HTTP 요청 파싱/응답, 정적 파일, 동적 처리(CGI), 에러 처리, robust I/O.
- **Phase 1 후반: keep-alive (D9 = B+).** 초반에는 `Connection: close`로 단순하게 동작하지만, 연결 루프와 버퍼는 keep-alive 가능하게 설계한다.
- HTTP 선택 기능: HEAD, Last-Modified/If-Modified-Since, OPTIONS, Range/206(단일 범위).
- 선택(비필수): Expect: 100-continue.
- 제외: 동시성, 프록시, chunked, TLS, gzip, directory listing, pipelining.

## 1. 결정 요약

| ID | 결정 | 상태 | 이유 |
|---|---|---|---|
| D1 | Phase 1 동적 처리 | **결정: CGI (C 프로그램)** | CSAPP 11.5.4와 일치, Phase 1 자체 완결, Phase 3와 역할 분리. CGI 프로그램도 C로 작성 |
| D6 | 설정 방식 | **결정: CLI (Phase 1) + Phase 3 설정 파일** | Phase 1은 설정이 적어 CLI로 충분. Phase 3 라우팅 규칙은 설정 파일이 적합 |
| D7 | 빌드 | **결정: Makefile** | 파일 수 적고 외부 의존성 없음. CMake는 과함 |
| D9 | Phase 1 keep-alive 시점 | **결정: B+** | 초반 close-only, 구조는 keep-alive 대비, 핵심 기능 후 활성화 |
| D10 | Phase 1 HTTP 기능 범위 | **결정:** HEAD, Last-Modified/If-Modified-Since, OPTIONS, Range/206(단일). 100-continue는 선택. ETag는 보류 | 실용 기능만, 비용/위험 통제 |
| D11 | Phase 1 요청 한계 | **결정: 커스텀 확정** | URI 4KB / 헤더 16KB / 100개 / 본문 8MB / 요청 20s / keep-alive idle 10s / max 100. CLI로 조정 |
| D12 | 정적 파일 정책 | **결정: A안** | `/dir`→301, index 없음→404, symlink docroot 안만, dotfile 차단, `index.html`만 |
| D13 | 테스트 전략 | **결정: 커스텀 단위 하네스 + 셸 통합** | 의존성 없음, unit/integration 분리. CMocka/Unity 미사용 |
| D14 | 이식성 목표 | **결정: macOS + Linux, POSIX 코어** | POSIX API 우선, 플랫폼 특화는 `#ifdef`/얇은 계층 |

## 2. 기능 요구사항

| ID | 요구사항 | 우선순위 | 검증 |
|---|---|---|---|
| FR-01 | `socket()` → `bind()` → `listen()` → `accept()` 표준 흐름 | Must | 서버 기동, `curl` 접속 |
| FR-02 | `bind` 실패, `accept` 블로킹/`EINTR`, 클라이언트 끊김 처리 | Must | 포트 충돌, 시그널, `nc` 끊기 |
| FR-03 | 요청 라인(메서드, URI, 버전) 파싱 | Must | 단위 테스트 |
| FR-04 | 헤더 파싱(Host, Content-Length, Connection 등) | Must | 단위 테스트 |
| FR-05 | POST 본문 처리, `Content-Length` 기반 경계 | Must | `curl -d`, `nc` |
| FR-06 | GET/POST 지원 | Must | `curl`, `nc` |
| FR-07 | 상태 라인, 표준 헤더, 본문 송신 | Must | `curl -v` |
| FR-08 | `Content-Type`, `Content-Length`, `Connection` 정확히 | Must | `curl -v` |
| FR-09 | docroot 기준 파일 읽기, MIME 매핑, 404/403/500 | Must | 브라우저, `curl` |
| FR-10 | 경로 탈출(`..`) 방어, docroot 밖 접근 차단 | Must | `curl --path-as-is` |
| FR-11 | 동적 처리 (D1) | Must | CGI/백엔드 호출 |
| FR-12 | CGI 환경변수, stdin/stdout, `waitpid`, fd 정리 | Must (D1=CGI) | POST CGI 테스트 |
| FR-14 | keep-alive: `Connection` 파싱, idle timeout, max requests, 버퍼 잔여 처리 | Must (D9) | 같은 연결로 2+ 요청 |
| FR-22 | HEAD: GET과 같은 헤더, 본문 생략 | Should | `curl -I` |
| FR-23 | Last-Modified / If-Modified-Since: 304 캐시 검증 | Should | `curl -H 'If-Modified-Since: ...'` |
| FR-24 | OPTIONS: `Allow` 헤더 응답 | Should | `curl -X OPTIONS` |
| FR-26 | Expect: 100-continue | 선택 | 큰 POST + `Expect` |
| FR-27 | Range / 206 (단일 범위) | Should | `curl -r 0-499` |
| FR-28 | chunked | Phase 3 | Phase 1은 501 + 연결 종료 |

## 3. 비기능 요구사항

| ID | 요구사항 | 검증 |
|---|---|---|
| NFR-01 | C + POSIX API만 사용, 금지 라이브러리 미사용 | 빌드/의존성 검사 |
| NFR-02 | Makefile 제공 | `make` |
| NFR-03 | 포트/docroot/cgi_dir를 CLI로 받기 | 실행 인자 확인 |
| NFR-04 | macOS/Linux에서 빌드·실행, POSIX 범위 또는 `#ifdef` | 양 환경 빌드 |
| NFR-05 | 3-way handshake, `SO_REUSEADDR`, blocking/non-blocking 이해 | 문서/테스트 |
| NFR-06 | RFC 7230~7235 최소 범위, 요청/헤더/본문 경계 | 파서 테스트 |
| NFR-09 | malloc/free 짝, 버퍼 오버플로 방지, 끊김 시 자원 회수 | ASan/valgrind |
| NFR-10 | SIGINT/SIGTERM 안전 종료, SIGPIPE 무시 | 시그널 테스트 |
| NFR-11 | 파서, MIME, URI 등 단위 테스트 | 테스트 실행 |
| NFR-12 | `curl`/브라우저 기능 테스트 | 테스트 로그 |
| NFR-14 | valgrind 또는 sanitizer | 클린 로그 |
| NFR-15 | 잘못된 요청, 큰 본문, 긴 URI, 끊김 등 엣지 케이스 | 테스트 케이스 |
| NFR-16 | README, 구조, 테스트 결과, 한계 문서화 | 문서 리뷰 |

## 4. 상세 요구사항

### 4.1 실행 / 설정 (D11 커스텀)

- `./server <port> <docroot> [cgi_dir]`
- **Phase 1은 CLI만 사용한다.** Phase 3에서 라우팅 규칙을 위한 설정 파일을 추가한다. (D6)
- 포트: 1~65535 검증.
- docroot: 존재하고 디렉토리인지 확인.
- cgi_dir: 선택. 없으면 동적 처리 비활성.
- **D11 확정값 (CLI로 조정 가능):**
  - URI 최대 4KB
  - 헤더 블록 최대 16KB
  - 헤더 개수 최대 100
  - 본문 최대 8MB
  - 요청 타임아웃 20초
  - keep-alive idle timeout 10초
  - keep-alive max requests 100
- CLI 플래그 예: `--max-uri`, `--max-header`, `--max-body`, `--timeout`, `--keepalive-idle`, `--keepalive-max`.
- 한계 적용:
  - URI 초과 → `414`
  - 헤더 블록/개수 초과 → `431`
  - 본문 초과 → `413`
  - 요청 타임아웃 → `408` 후 연결 종료
  - 초과/타임아웃 시 본문을 끝까지 읽지 않고 연결을 닫는다.
  - `Content-Length`를 보고 `malloc(Content-Length)`부터 하지 않는다.

### 4.2 TCP 서버

- **FR-01**
  - `socket(AF_INET, SOCK_STREAM, 0)`
  - `SO_REUSEADDR` 설정 후 `bind`.
  - `listen(fd, SOMAXCONN)`.
  - `accept` 루프.
- **FR-02**
  - `EINTR`, `ECONNABORTED`는 재시도/계속.
  - `EMFILE` 같은 fd 고갈은 로그 후 백오프.
  - 클라이언트가 끊겨도 프로세스가 죽지 않아야 함.
- **NFR-10**
  - `SIGPIPE` 무시.
  - `SIGINT`/`SIGTERM` 시 새 `accept` 중단, 현재 연결 마무리 후 종료.

### 4.3 HTTP 요청 파싱

- **FR-03**
  - `\r\n\r\n`까지 읽어 헤더 블록 구성.
  - 요청 라인을 `method SP target SP version`으로 파싱.
  - 형식 오류 → `400`.
- **FR-04/NFR-06**
  - 헤더 이름은 대소문자 무시.
  - 값 앞뒤 OWS 제거.
  - 중복 헤더 정책:
    - `Content-Length` 중복: 값이 같으면 허용, 다르면 `400`.
    - `Content-Length` + `Transfer-Encoding`: `400`.
    - `Transfer-Encoding: chunked`: Phase 1에서 `501` + 연결 종료.
  - `Host`: HTTP/1.1에서 필수. 없으면 `400`.
- **FR-05**
  - POST에서 `Content-Length` 필수. 없으면 `411` 또는 `400`.
  - 본문은 정확히 `Content-Length` 바이트만 읽기.
  - 초과 → `413`.
- **FR-06**
  - GET, POST만 지원.
  - 그 외 메서드: `501 Not Implemented` 또는 `405 Method Not Allowed` + `Allow`.
- **URI**
  - origin-form(`/path?query`)만 지원.
  - 쿼리 분리 후 파일 경로에서 제거.
  - 퍼센트 디코딩 후 정규화.
  - `..`로 docroot 탈출 시 `403`.
  - 너무 길면 `414`.
- **버전**
  - HTTP/1.0, HTTP/1.1만 지원.
  - 그 외 `505`.
- **Connection (D9 = B+)**
  - **Phase 1 초반:** `should_close = 1`로 항상 `Connection: close` 응답 후 종료.
  - **Phase 1 후반:** keep-alive 활성화.
    - HTTP/1.1: 기본 keep-alive. `Connection: close`면 응답 후 종료.
    - HTTP/1.0: 기본 close. `Connection: keep-alive`면 유지.
    - idle timeout(10초, 설정 가능) 초과 시 종료.
    - max requests(100, 설정 가능) 초과 시 종료.
    - 에러, EOF, 타임아웃 시 종료.
    - 버퍼에 남은 바이트는 다음 요청의 시작으로 이어서 파싱한다.
  - 단일 스레드/블로킹에서 idle 연결이 다른 클라이언트를 막지 않도록 타임아웃을 반드시 둔다.

### 4.4 HTTP 응답

- **FR-07/FR-08**
  - 상태 라인 + 헤더 + 빈 줄 + 본문.
  - 모든 응답에 정확한 `Content-Length`.
  - `Content-Type`은 MIME 매핑 결과.
  - `Connection`: 초반에는 `close`, 후반 keep-alive에서는 요청/버전에 따라 `close` 또는 `keep-alive`.
  - `Date`, `Server`는 선택.
- **에러 응답**
  - `400`, `403`, `404`, `405`, `408`, `413`, `414`, `431`, `500`, `501`, `505`.
  - HTML body + 정확한 `Content-Length`.
  - 내부 경로/스택트레이스 노출 금지.

### 4.5 정적 파일 서빙

- **FR-09**
  - URI → docroot 기준 경로.
  - `/` → `index.html`.
  - MIME 매핑:
    - `.html` `text/html`
    - `.css` `text/css`
    - `.js` `text/javascript`
    - `.png` `image/png`
    - `.jpg`/`.jpeg` `image/jpeg`
    - `.gif` `image/gif`
    - `.svg` `image/svg+xml`
    - `.json` `application/json`
    - `.txt` `text/plain`
    - `.ico` `image/x-icon`
    - `.pdf` `application/pdf`
    - 기본 `application/octet-stream`
  - `open` → `fstat` → `S_ISREG` 확인.
  - `st_size`로 `Content-Length` 설정.
  - 64KB 단위로 스트리밍 (전체 malloc 금지).
  - `EACCES` → `403`, `ENOENT`/`ENOTDIR` → `404`, 그 외 → `500`.
- **FR-10**
  - 퍼센트 디코딩 후 `..` 세그먼트 정규화.
  - `realpath` 결과가 docroot prefix인지 확인.
  - NUL 바이트 포함 URI 거부.
  - symlink는 docroot 안일 때만 허용.
- **D12 (결정: A안)**
  - `/dir` 요청 → `301 Moved Permanently` + `Location: /dir/` (쿼리 유지).
  - `/dir/` → `index.html` 서빙.
  - index 없는 디렉토리 → `404 Not Found`.
  - directory listing → 생성하지 않음 (제외).
  - dotfile/dotdir → 차단. 응답은 `404`로 통일해 존재를 숨긴다.
  - symlink → `realpath()` 결과가 docroot 안일 때만 허용. 밖이면 `403`.
  - index 파일은 `index.html`만 사용 (`index.htm` 미지원).

### 4.6 동적 처리 (D1: CGI, C 프로그램)

> D1 확정: CGI. CGI 프로그램은 C로 작성한다. 서버는 C로 구현하고, CGI는 외부 실행 파일로 둔다.

- **FR-11/FR-12**
  - `/cgi-bin/*` 경로를 cgi_dir로 매핑.
  - `fork()` + `execve()` (절대 경로, shell 미사용).
  - 환경변수:
    - `GATEWAY_INTERFACE=CGI/1.1`
    - `REQUEST_METHOD`
    - `QUERY_STRING`
    - `CONTENT_LENGTH`
    - `CONTENT_TYPE`
    - `SCRIPT_NAME`
    - `SERVER_PROTOCOL`
    - `SERVER_PORT`
    - `REMOTE_ADDR`
  - POST 본문 → 자식 stdin.
  - CGI stdout → 클라이언트.
  - `waitpid()`로 자식 회수.
  - `FD_CLOEXEC`로 listen/conn fd 상속 방지.
  - 큰 본문에서 stdin/stdout 교착 방지 (임시 파일 또는 `poll`).
  - CGI 타임아웃은 Should.
  - cgi_dir 밖 실행 금지.
- **CGI 프로그램 (C)**
  - `cgi-bin/echo.c`, `cgi-bin/time.c` 등.
  - Makefile `cgi` 타깃으로 빌드.
  - CGI 프로그램은 env/stdin을 읽고, `Content-Type` + 빈 줄 + 본문을 stdout에 출력.
  - 런타임 의존성 없음.

### 4.7 Robust I/O / 시그널

- **NFR-09/NFR-10**
  - `readn`/`writen` 루프.
  - short count, `EINTR`, EOF, `EPIPE` 처리.
  - `SIGPIPE` 무시.
  - 모든 경로에서 fd/malloc 해제.
  - `accept`, `read`, `write`의 에러를 로그.

### 4.8 로깅

- **NFR-16**
  - 접근 로그: 클라이언트 IP, 메서드, 경로, 상태 코드, 바이트 수.
  - 에러 로그: stderr, 필요하면 파일.
  - 민감 정보(Authorization 등) 로그 금지.

### 4.9 HTTP 선택 기능 (D10)

#### 확정
- **FR-22 HEAD**
  - GET과 같은 응답 헤더를 보내고 본문은 생략.
  - `Content-Length`는 GET과 동일.
  - 검증: `curl -I`
- **FR-23 Last-Modified / If-Modified-Since**
  - GET 응답에 `Last-Modified`를 붙인다.
  - `If-Modified-Since`가 있고 파일이 그 이후로 안 바뀌었으면 `304 Not Modified`(본문 없음).
  - 날짜는 IMF-fixdate.
  - **ETag는 Phase 1에서 채택하지 않는다.** Last-Modified만 사용.
  - 검증: `curl -I`, `curl -H 'If-Modified-Since: ...'`
- **FR-24 OPTIONS**
  - `Allow: GET, POST, HEAD, OPTIONS`, `Content-Length: 0`.
  - 검증: `curl -X OPTIONS`
- **FR-27 Range / 206 (단일 범위)**
  - `Range: bytes=start-end`, `bytes=start-`, `bytes=-suffix`.
  - `206 Partial Content` + `Content-Range: bytes start-end/total`.
  - `Accept-Ranges: bytes`.
  - 잘못된 범위 → `416` + `Content-Range: bytes */total`.
  - 여러 범위(`multipart/byteranges`)는 미지원.
  - Should: 시간이 부족하면 Phase 3로 미룰 수 있음.

#### 선택 (비필수)
- **FR-26 Expect: 100-continue**
  - HTTP/1.1에서 본문을 읽기 전에 `100 Continue` 또는 최종 에러.
  - 큰 POST 거절 시 본문 전송을 아낀다.
  - **Phase 1 필수 아님.** 인수 기준에서 제외.

#### 보류 / Phase 3
- **FR-25 ETag / If-None-Match**: Phase 1 미채택. Phase 2/3에서 재검토.
- **FR-28 chunked**: Phase 1은 `501` + 연결 종료. Phase 3 프록시에서 필수.
- **gzip**: Phase 1 제외. Phase 3에서 결정.
- **Directory listing**: 제외.
- **Pipelining**: 제외.

### 4.10 이식성 (D14)

- **결정: macOS + Linux, POSIX 코어.**
- 코어 구현은 POSIX API만 사용한다.
  - 소켓: `socket`, `bind`, `listen`, `accept`, `connect`, `read`, `write`, `close`
  - 주소: `getaddrinfo`, `inet_pton`, `inet_ntop`
  - 파일: `open`, `read`, `write`, `fstat`, `lseek`
  - 프로세스: `fork`, `execve`, `waitpid`, `pipe`
  - 시그널: `sigaction`, `signal`
- 플랫폼 특화 API는 코어에 넣지 않는다. 필요하면 `#ifdef` 또는 얇은 `platform/` 계층으로 분리한다.
  - Linux 전용: `epoll`, `accept4`, `sendfile`
  - macOS/BSD 전용: `kqueue`, `SO_NOSIGPIPE`
  - 공통 `SIGPIPE` 대응: `signal(SIGPIPE, SIG_IGN)` (POSIX)
- 파일 전송은 `read`/`write` 루프를 기본으로 한다. `sendfile`은 선택 최적화.
- Phase 2 이벤트 루프를 고르면 `poll`을 기본으로 하고, `kqueue`/`epoll`은 `#ifdef`로 감싼다.
- 빌드 플래그 예: `-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra`.
- 테스트: macOS에서 기본 테스트, Linux는 Docker/CI에서 `make` + `make test`/`make integration`으로 확인.
- 플랫폼별 코드가 있으면 README에 위치와 이유를 문서화한다.

## 5. 테스트

> 테스트 케이스 목록과 테스트 우선 순서는 [phase1-test-plan.md](phase1-test-plan.md)에 있다. **구현 전에 해당 테스트를 먼저 작성한다.**

### D13 — 테스트 구성 (결정)

- **단위 테스트**: `tests/test_*.c`, 작은 `TEST_ASSERT` 매크로 기반. 네트워크 없이 순수 함수만.
- **통합 테스트**: `tests/integration.sh`. 서버를 ephemeral port로 띄우고 `curl`/`nc`로 검증.
- **부하 테스트**: `make load` (`ab -n 1000 -c 1`, `ab -k`).
- **메모리 검사**: `make sanitize`로 ASan/UBSan 빌드 후 테스트.
- **Makefile 타깃**: `make test`, `make integration`, `make sanitize`, `make load`.
- **테스트 픽스처**: `tests/fixtures/`에 index/no-index 디렉토리, dotfile, symlink, 큰 파일, 바이너리 파일.
- CMocka/Unity는 사용하지 않는다. mock이 많이 필요해지면 재검토.

- 단위 테스트:
  - 요청 라인 파서
  - 헤더 파서
  - URI 디코딩/정규화
  - MIME 매핑
  - 에러 응답 빌더
  - Range 파서
- 통합 테스트:
  - `curl` GET 정적 파일
  - `curl -I` HEAD: 본문 없음, `Content-Length` 동일
  - `curl -H 'If-Modified-Since: ...'` → `304`
  - `curl -X OPTIONS` → `200` + `Allow`
  - `curl -r 0-499` → `206` + `Content-Range`
  - 잘못된 Range → `416`
  - `curl` POST CGI (D1=CGI인 경우)
  - `404`, `403`, `400`, `405`, `408`, `413`, `414`, `431`, `500`, `501`, `505`
  - 경로 탈출
  - 큰 파일
  - 잘못된 요청
  - 클라이언트 중간 끊기
  - keep-alive: 같은 연결로 2+ 요청
  - HTTP/1.1 기본 keep-alive, HTTP/1.0 기본 close
  - `Connection: close` → 응답 후 종료
  - idle timeout 후 종료
  - max requests 후 종료
- 부하 테스트:
  - `ab -n 1000 -c 1` (초반, close-only 기준)
  - `ab -k -n 1000 -c 1` (후반, keep-alive 기준)
- 메모리:
  - `-fsanitize=address,undefined`
  - valgrind는 Linux/Docker에서 선택.

## 6. 인수 기준

- [ ] `curl -v http://127.0.0.1:8080/index.html` → `200`, 올바른 MIME/본문
- [ ] `curl -I http://127.0.0.1:8080/index.html` → `200`, 본문 없음, `Content-Length` 동일
- [ ] `curl -H 'If-Modified-Since: ...'` → `304`
- [ ] `curl -X OPTIONS http://127.0.0.1:8080/index.html` → `200` + `Allow`
- [ ] (Should) `curl -r 0-499 http://127.0.0.1:8080/big.bin` → `206` + `Content-Range`
- [ ] (Should) 잘못된 Range → `416` + `Content-Range: bytes */<size>`
- [ ] (D1=CGI) `curl -v -d 'hello=world' http://127.0.0.1:8080/cgi-bin/echo` → CGI 응답 정상
- [ ] `404`, `403`, `400`, `405`, `408`, `413`, `414`, `431`, `500`, `501`, `505` 정상
- [ ] `GET /../../etc/passwd` → `403`/`404`
- [ ] **D12:** `/images` → `301` + `Location: /images/`
- [ ] **D12:** `/images/` → `200`, `images/index.html`
- [ ] **D12:** `/docs/` (index 없음) → `404`
- [ ] **D12:** `/.env` → `404` (dotfile 차단)
- [ ] **D12:** docroot 안 symlink → `200`, docroot 밖 symlink → `403`
- [ ] 큰 파일이 메모리 폭발 없이 스트리밍됨
- [ ] malformed 요청에도 프로세스가 죽지 않음
- [ ] `-fsanitize=address,undefined` 클린
- [ ] `make test` (단위) 통과
- [ ] `make integration` 통과
- [ ] `make load` 결과 기록
- [ ] `ab -n 1000 -c 1` 통과 (초반, close-only 기준)
- [ ] **D9=B+ keep-alive:** 같은 연결로 2+ 요청 처리
- [ ] **D9=B+ keep-alive:** HTTP/1.1 기본 keep-alive, HTTP/1.0 기본 close
- [ ] **D9=B+ keep-alive:** `Connection: close` → 응답 후 종료
- [ ] **D9=B+ keep-alive:** idle timeout 후 종료
- [ ] **D9=B+ keep-alive:** max requests 후 종료
- [ ] **D9=B+ keep-alive:** `ab -k -n 1000 -c 1` 통과
- [ ] **D11 한계:** URI 4KB 초과 → `414`
- [ ] **D11 한계:** 헤더 16KB 초과 → `431`
- [ ] **D11 한계:** 본문 8MB 초과 → `413`
- [ ] **D11 한계:** 요청 타임아웃 → `408`/종료
- [ ] README/테스트 결과 문서화
- [ ] **D14:** macOS와 Linux에서 `make` 성공
- [ ] **D14:** 코어에 플랫폼 전용 API가 없거나 `#ifdef`/`platform/`로 분리됨
- [ ] **D14:** 양쪽에서 `make test`/`make integration` 통과 (Linux는 Docker/CI)

> 선택(비필수): `Expect: 100-continue`는 Phase 1 인수 기준이 아니다. 구현하면 큰 POST에서 curl의 약 1초 대기를 줄일 수 있다.

## 7. 범위 제외 (Phase 1)

- 동시성
- Proxy/Reverse Proxy
- `Transfer-Encoding: chunked`
- TLS, HTTP/2, HTTP/3
- 압축(gzip), 캐싱, 가상 호스팅
- 전체 RFC 7230~7235 준수 (최소 범위만)
- HTTP pipelining은 명시적으로 지원하지 않음 (버퍼 잔여 처리는 함)

## 8. 열린 결정

- Phase 1 범위의 결정은 모두 확정됐다.
- Phase 3에서 설정 파일 형식/라우팅 규칙을 설계한다. (D6)

## 9. Phase 2로 넘기는 것

- `accept` 루프 + `should_close` 구조.
- 연결별 버퍼/요청 수/마지막 활동 시각 상태.
- 파서/응답/정적/CGI 모듈 인터페이스.
- 시그널 처리와 graceful shutdown 뼈대.
- keep-alive 동작과 테스트.

## 10. 결정 로그

### D1 — Phase 1 동적 처리: CGI, C 프로그램 (확정)

- **결정일:** 2026-11-20
- **결정:** Phase 1은 CGI로 동적 요청을 처리한다. CGI 프로그램은 C로 작성한다.
- **검토한 대안:**
  - **백엔드 전달:** Phase 3 프록시와 코드가 겹치고, Phase 1에 별도 백엔드 프로세스가 필요하다.
  - **내장 엔드포인트:** 가장 단순하지만 과제 예시(CGI/백엔드)와 거리가 있고, 동적 처리의 깊이가 얕다.
- **선택 이유:**
  - CSAPP 11.5.4/Tiny의 CGI 흐름과 일치한다.
  - Phase 1을 자체 완결로 유지한다. 별도 백엔드 프로세스가 필요 없다.
  - Phase 3에서 프록시/백엔드 전달을 다루므로, 두 경험을 모두 하게 된다.
  - **런타임 의존성이 없다:** `make`로 `server`와 `cgi-bin/echo`를 같이 빌드하면 끝이다. Python/Shell은 실행 환경에 의존한다.
  - **에러 처리/메모리 모델이 일관된다:** 서버와 CGI가 같은 C 스타일을 쓴다.
  - **CGI 프로그램은 작게 유지할 수 있다:** `echo`, `time`, `search` 정도면 C로도 부담이 적다.
  - CGI의 언어 독립성은 README에 설명하되, 이 프로젝트에서는 C로 통일한다.
- **영향:**
  - `src/cgi.c`(러너)와 `cgi-bin/*.c`(프로그램)가 추가된다.
  - `fork`/`execve`/`pipe`/`dup2`/`waitpid`, 환경변수, fd 정리, 타임아웃, 보안이 필요하다.
  - `make`에 `cgi` 타깃을 추가한다.
- **재검토 조건:**
  - CGI 러너의 복잡도가 Phase 1 일정을 과도하게 지연시키면, 내장 엔드포인트로 축소하는 방안을 검토한다.
  - 백엔드/프록시가 필요해지면 Phase 3에서 다룬다.

### D6 — 설정 방식: CLI (Phase 1) + Phase 3 설정 파일 (확정)

- **결정일:** 2026-11-20
- **결정:** Phase 1은 CLI만 사용한다. Phase 3에서 라우팅 규칙/백엔드/타임아웃을 위한 설정 파일을 추가한다.
- **검토한 대안:**
  - **CLI only:** Phase 1은 단순하지만 Phase 3 라우팅 규칙이 길어져 관리가 어렵다.
  - **설정 파일 only:** Phase 1부터 파서가 필요하고, 작은 설정에는 과하다.
  - **둘 다 (처음부터):** 유연하지만 Phase 1 작업이 늘어난다.
- **선택 이유:**
  - Phase 1 설정은 포트/docroot/cgi_dir/한계값 정도라 CLI로 충분하다.
  - Phase 3는 경로/호스트 라우팅, 백엔드 목록, 타임아웃, 정적 서빙 정책 등 규칙이 많아 설정 파일이 적합하다.
  - 단계적으로 추가하면 Phase 1에서 파서를 만들지 않아도 된다.
- **영향:**
  - Phase 1: `./server <port> <docroot> [cgi_dir]` + CLI 플래그.
  - Phase 3: 설정 파일 + CLI override (CLI > 설정 파일 > 기본값).
  - 설정 파일 형식은 Phase 3 설계에서 결정한다.
- **재검토 조건:**
  - Phase 3 라우팅 규칙이 단순하면 CLI만으로 유지할 수 있다.
  - 설정 파일 형식/우선순위는 Phase 3 설계에서 정한다.

### D7 — 빌드: Makefile (확정)

- **결정일:** 2026-11-20
- **결정:** Makefile을 사용한다.
- **검토한 대안:**
  - **CMake:** IDE/의존성 관리/크로스플랫폼에 좋지만, 작은 C 프로젝트에는 보일러플레이트가 크다.
- **선택 이유:**
  - 파일 수가 적고 외부 라이브러리가 없다.
  - macOS/Linux에서 추가 도구 없이 동작한다.
  - `all`, `server`, `cgi`, `test`, `integration`, `sanitize`, `load`, `clean` 타깃을 직접 관리하면 충분하다.
- **영향:**
  - Phase 3에서도 같은 Makefile을 확장한다.
  - 의존성 추적은 수동으로 관리한다.
