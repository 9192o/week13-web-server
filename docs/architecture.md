# 아키텍처 — Phase 1

## 모듈

```text
main
 ├─ config
 └─ server
      ├─ http_request
      ├─ http_response
      ├─ static_file
      ├─ cgi
      └─ util
```

| 모듈 | 역할 |
|---|---|
| `main.c` | CLI 파싱, `config` 초기화, 서버 시작 |
| `config.c/h` | 포트/docroot/cgi_dir/한계값/keep-alive 설정, 검증 |
| `server.c/h` | `socket`/`bind`/`listen`/`accept`, 연결 루프, keep-alive, 시그널 |
| `http_request.c` | 요청 라인/헤더/본문 파싱, URI 정규화, 헤더 조회 |
| `http_response.c` | 상태 라인/헤더/본문, 에러/리다이렉트/OPTIONS 응답 |
| `static_file.c` | docroot 매핑, index, MIME, 경로 탈출/symlink/dotfile, Range/304, 스트리밍 |
| `cgi.c` | `/cgi-bin/*` 실행, 환경변수, stdin/stdout, 타임아웃 |
| `util.c` | robust I/O, 로깅, MIME, HTTP 날짜 |

## 요청 생명주기

```text
accept()
  → connection loop
      → read headers until \r\n\r\n (or limit)
      → http_request_parse_buffer()
      → http_request_read_body()
      → options / cgi / static_file
      → response headers + body
      → keep-alive? reset request + carry leftover : close
```

### 버퍼 소유권

- `server.c`가 연결별 `buf[16384]`를 들고 있다.
- `http_request`의 헤더 이름/값은 이 버퍼를 가리키는 **zero-copy 포인터**다.
- `header_len`은 헤더 블록이 끝난 위치(본문 시작)를 나타낸다.
- `http_request_read_body`는:
  1. 버퍼에 이미 들어온 본문 바이트를 복사하고
  2. 모자란 만큼 `readn_stop`으로 읽는다. 종료 플래그가 세워지면 중단한다.
- keep-alive에서 본문 뒤에 남은 바이트는 `memmove`로 버퍼 앞으로 옮겨 다음 요청으로 넘긴다.

## keep-alive

- HTTP/1.1 기본 keep-alive, HTTP/1.0 기본 close.
- `Connection: close`면 응답 후 종료.
- `keepalive_max_requests`에 도달하면 `Connection: close`.
- 종료 중(`g_stop`)이면 `Connection: close`로 응답하고 다음 요청을 읽지 않는다.
- 다음 요청을 기다릴 때는 `keepalive_idle_sec`, 요청이 시작되면 `request_timeout_sec` 타임아웃을 사용한다.
- idle timeout은 응답 없이 close, 부분 요청 timeout은 `408`.
- 에러 응답은 항상 close.

## 정적 파일 보안

- URI를 퍼센트 디코딩하고 `.`/`..`를 정규화한다.
- docroot 탈출은 `403`.
- dotfile/dotdir 세그먼트는 `404`.
- 요청 경로와 symlink 해석 후 경로 둘 다 dotfile 검사.
- `realpath(docroot)`와 `realpath(filepath)`를 비교해 docroot 밖 symlink는 `403`.
- 디렉터리는 `/dir` → `301 /dir/`, `/dir/` → `index.html`, index 없으면 `404`.
- 큰 파일은 64KB 청크로 스트리밍한다.

## CGI

- `/cgi-bin/*`를 cgi_dir 아래 실행 파일로 매핑.
- `realpath`로 cgi_dir 밖 실행을 차단.
- `fork` + `execve`.
- 요청 정보는 환경변수로 전달:
  - `GATEWAY_INTERFACE`, `REQUEST_METHOD`, `QUERY_STRING`, `CONTENT_LENGTH`, `CONTENT_TYPE`, `SCRIPT_NAME`, `SERVER_PROTOCOL`, `SERVER_PORT`, `REMOTE_ADDR`
- POST 본문은 임시 파일에 써서 child stdin으로 연결한다 (파이프 교착 방지).
- child stdout을 `poll`로 읽고, `request_timeout_sec`을 초과하면 child를 종료하고 `504`.
- CGI 출력의 헤더를 파싱해 HTTP 응답으로 변환한다. `Content-Length`는 서버가 계산한다.
- 출력은 1MB로 제한한다.

## 시그널 / 자원

- `SIGPIPE` 무시 → 쓰기 중 클라이언트가 끊겨도 서버 생존.
- `SIGINT`/`SIGTERM` → 첫 시그널은 `g_stop` 플래그, 두 번째 시그널은 `_exit(1)` 강제 종료.
- 종료 시 헤더/본문을 읽는 중인 요청은 중단하고, 이미 파싱된 요청의 응답은 마무리한다. keep-alive 응답에는 `Connection: close`를 붙인다.
- `SO_RCVTIMEO`/`SO_SNDTIMEO`으로 읽기/쓰기 대기 시간을 제한한다.
- Phase 1은 요청 처리 중 `listenfd`를 즉시 닫을 수 없어 backlog 연결이 RST될 수 있다 (Phase 2에서 개선).
- `listenfd`에 `FD_CLOEXEC`.
- 연결마다 `connfd`, 정적 파일 fd, CGI 파이프/임시 파일을 모든 경로에서 닫는다.
- fd 누수는 `make fdtest`로 확인한다.

## 테스트 / 품질

- 단위 테스트: 파서, URI, MIME, 날짜, 응답, Range.
- 통합 테스트: 정적/D12/D10/D11/CGI/keep-alive.
- ASan/UBSan: `make sanitize`.
- fd 누수: `make fdtest`.
- 부하: `make load` (`ab`).
- 자세한 내용: [phase1-test-plan.md](phase1-test-plan.md), [quality-gate.md](quality-gate.md),
  실행 흐름: [function_flow.md](function_flow.md), 함수별 설명: [function_reference.md](function_reference.md).

## Phase 2 준비

- `server.c`의 연결 루프와 연결별 상태(`pending`, `requests`)가 동시성 모델로 확장될 지점이다.
- 전역 상태는 `g_stop`과 설정뿐이라, 연결별 컨텍스트를 스레드/이벤트 루프에 넘기기 쉽다.
- Phase 2에서 `fork`/pthread 스레드 풀/`poll`·`kqueue`·`epoll` 중 하나를 선택한다.
