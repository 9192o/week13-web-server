# 함수 흐름 — Phase 1

이 문서는 서버가 **어떤 순서로 돌아가는지** 읽는 가이드다. `main`에서 시작해 `accept`, 연결 처리, keep-alive까지 따라간다.
함수 하나하나의 상세 설명은 [function_reference.md](function_reference.md)에 모아 두었다.

- 여기서는 "왜 이 순서인가", "이 상태가 무엇을 뜻하는가"를 중심으로 본다.
- 함수별 `파일:라인`, 반환형, 호출자는 레퍼런스에서 찾는다.
- `docs/architecture.md`가 모듈 구조를, 이 문서가 실행 흐름을 설명한다.

---

## 1. 전체 호출 흐름

```text
main
├─ config_init
├─ config_parse_cli
│   ├─ copy_path
│   ├─ parse_int_arg
│   └─ parse_size_arg
├─ config_validate
└─ server_run
    ├─ setup_signals
    │   └─ handle_signal                (SIGINT/SIGTERM 수신 시)
    ├─ create_listen_socket
    └─ while (!g_stop)
        ├─ accept
        └─ handle_connection
            ├─ has_header_end
            ├─ set_send_timeout
            ├─ set_recv_timeout
            ├─ http_request_init
            ├─ http_request_parse_buffer
            │   ├─ find_crlf
            │   ├─ parse_request_line
            │   │   ├─ parse_method
            │   │   └─ parse_version
            │   ├─ parse_headers
            │   │   ├─ trim_ows
            │   │   ├─ ci_equal / ci_contains
            │   │   └─ parse_content_length
            │   └─ parse_uri
            │       ├─ decode_uri
            │       │   └─ hexval
            │       └─ normalize_path
            ├─ http_request_read_body
            │   └─ readn_stop
            ├─ 라우팅
            │   ├─ http_response_send_options
            │   ├─ cgi_handle
            │   │   ├─ path_within_root
            │   │   ├─ pipe / tmpfile / fork
            │   │   ├─ set_cgi_env
            │   │   ├─ poll
            │   │   └─ send_cgi_response
            │   └─ static_file_handle
            │       ├─ has_dotfile_segment
            │       ├─ status_from_errno
            │       ├─ path_within_root
            │       ├─ http_date_format / http_date_parse
            │       ├─ http_request_get_header
            │       ├─ parse_range_header
            │       │   └─ parse_size_value
            │       ├─ send_range_error
            │       ├─ http_response_send_headers
            │       └─ send_file
            ├─ http_request_free
            └─ close(connfd)
```

### 요청 하나의 상태 흐름

```text
accept
  → 헤더 끝(\r\n\r\n)까지 읽기
  → 헤더 파싱 (실패 시 400/414/431/505)
  → 본문 읽기 (실패 시 408/413)
  → 라우팅 (OPTIONS / CGI / 정적)
  → 응답 전송
  → keep-alive면 pending 바이트 정리 후 반복
  → 아니면 close
```

---

## 2. 모듈 지도 — 나머지 .c는 어디에 쓰이나

1절이 **함수 단위** 호출 그래프라면, 여기는 **파일 단위** 지도다.
`src/*.c` 7개는 전부 하나의 `server` 실행 파일로 링크된다 (`Makefile:7`).
`main.c`와 `server.c`만 "흐름"을 알고, 나머지는 `server.c`가 필요할 때 부르는 부품이다.

### 요청 시간축에서의 위치

```text
[시작 전]   config.c        ← main만 사용
[accept]
[요청 읽기]  http_request.c  ← server가 호출
[라우팅]     static_file.c   ← server가 호출
             cgi.c           ← server가 호출
[응답]       http_response.c ← server + 두 핸들러가 호출
[전 구간]    util.c          ← 거의 모든 모듈이 호출
```

### 모듈 의존 관계

```text
                    main.c
                   ╱      ╲
             config.c      server.c ──────────────┐
                              │  │  │              │
              ┌───────────────┘  │  └──────────┐   │
              ▼                  ▼             ▼   ▼
       http_request.c     static_file.c      cgi.c │
              │                  │             │   │
              └──────────┬───────┴─────────────┘   │
                         ▼                         │
                  http_response.c ◄────────────────┘
                         │
                         ▼
                      util.c
```

- 화살표는 include/호출 방향이다.
- **순환 의존이 없다.** `util → http_response → static_file → server` 방향으로만 흐르는 DAG라, 모듈을 따로 테스트할 수 있다.
- `server.c`는 `http_response.c`도 직접 부른다. `OPTIONS` 응답과 에러 응답은 핸들러를 거치지 않기 때문이다.

### 모듈별 역할

| 파일 | 누가 부르나 | 역할 | 없으면 |
|---|---|---|---|
| `config.c` | `main` | CLI/기본값/검증 | 포트·docroot를 못 정함 |
| `http_request.c` | `server` | 바이트 → 요청 구조체 | 헤더/URI 파싱 불가 |
| `http_response.c` | `server`, `static_file`, `cgi` | 상태·헤더·본문 전송 | 응답 조립이 곳곳에 중복 |
| `static_file.c` | `server` | 정적 파일 서빙 | 파일 서빙 불가 |
| `cgi.c` | `server` | 외부 프로그램 실행 | 동적 처리 불가 |
| `util.c` | 거의 모든 모듈 | I/O·날짜·MIME·로그 | 부분 읽기/쓰기 버그 |

#### `config.c` — 시작할 때만

- **호출자는 `main.c`뿐이다.** `server.c`는 `config_t`를 받기만 하고 config 함수는 부르지 않는다.
- `config_init` → `config_parse_cli` → `config_validate`가 프로세스 수명 중 딱 한 번 실행된다.
- 서버가 뜬 뒤에는 이 파일의 코드는 다시 실행되지 않는다. 그래서 "설정"과 "구동"이 분리된다.
- Phase 3에서 설정 파일이 생기면 이 파일에 함수가 하나 늘고, `server.c`는 건드리지 않아도 된다.

#### `http_request.c` — 요청이 도착할 때마다

- **호출자는 `server.c`의 `handle_connection` 하나뿐이다.**
- 입력은 `buf`(바이트), `len`, `cfg`; 출력은 `http_request_t`다.
- 이 구조체가 이후 모든 모듈의 공용 언어다. `static_file.c`는 `req.method/path/query`를, `cgi.c`는 `req.method/query/body`를 읽는다.
- "HTTP를 아는 지식"이 이 파일에 몰려 있다. 정적 파일 핸들러는 HTTP 헤더를 직접 파싱하지 않는다.
- Phase 3 프록시에서도 이 파서를 재사용한다. 요청을 파싱해 백엔드로 다시 보내야 하기 때문이다.

#### `http_response.c` — 응답을 내보낼 때마다

- **호출자는 세 곳**: `server.c`(OPTIONS/에러), `static_file.c`(파일 응답/304/301/416), `cgi.c`(CGI 출력 변환).
- "HTTP 응답을 만드는 지식"이 여기 몰려 있다. 상태 라인 형식, reason phrase, `Content-Length`, `Connection` 헤더.
- 핸들러는 상태 코드와 헤더 목록만 정하고, 바이트 조립은 이 모듈에 맡긴다.
- `http_status_reason`이 여기 있는 이유도 같다. 상태 코드 → 문자열 변환이 한곳에 있어야 일관성이 깨지지 않는다.

#### `static_file.c` — 정적 라우팅을 탔을 때

- **호출자는 `server.c` 라우팅의 `else` 가지 하나다.**
- `req.path`를 docroot 기준 파일 경로로 바꾸고, dotfile/symlink/탈출 검사를 하고, `open`/`stat`/스트리밍을 한다.
- "파일 시스템을 아는 지식"이 여기 몰려 있다. `http_request.c`는 경로 정규화까지만 하고 실제 파일 존재는 모른다.
- 이 파일이 없으면 CGI만 있는 서버가 된다.

#### `cgi.c` — `/cgi-bin/` 라우팅을 탔을 때

- **호출자는 `server.c` 라우팅의 `else if` 가지 하나다.**
- `fork`/`execve`/`pipe`/`poll`/`waitpid` — "프로세스를 다루는 지식"이 여기 몰려 있다.
- `http_request.c`가 만든 구조체를 환경변수와 stdin으로 풀어주는 번역기다.
- 이 파일이 없으면 동적 요청은 404가 된다. 라우팅 자체가 `cgi_dir`이 비어 있으면 꺼지기 때문이다.

#### `util.c` — 전 구간에서 조금씩

- **호출자는 거의 모든 모듈**이다. `server.c`(로그/타임아웃), `http_request.c`(`readn_stop`), `http_response.c`(`writen`), `static_file.c`(MIME/날짜/`writen`), `cgi.c`(`writen`).
- "여러 모듈이 똑같이 필요로 하는 바닥 지식"이다. 부분 읽기/쓰기 루프, IMF-fixdate 변환, 확장자→MIME.
- 이게 없으면 `read`/`write` 루프가 모듈마다 복사되고, 한 곳의 버그를 고쳐도 다른 곳에 남는다.
- `readn_stop`/`writen`이 특히 중요하다. 소켓은 "한 번에 다 읽고 쓰는" 것을 보장하지 않는다.

#### `main.c` — 24줄짜리 지휘자

- **아무 일도 직접 하지 않는다.** 설정 3단계와 `server_run`을 순서대로 엮기만 한다.
- 이 파일이 얇은 덕분에 `server.c`를 테스트하거나 Phase 2에서 바꾸기가 쉽다.

### 실행 파일 경계

`src/` 밖의 `.c`는 같은 바이너리에 들어가지 않는다.

| 위치 | 실행 파일 | 링크 시점 |
|---|---|---|
| `src/*.c` | `server` 하나 | 빌드할 때 전부 링크 |
| `cgi-bin/echo.c`, `sleep.c`, `crash.c` | 각각 별도 실행 파일 | 빌드할 때 각각 컴파일 |
| `tests/*.c` | `run_tests` 하나 | 테스트할 때만 링크 |

- **`cgi-bin/*.c`는 `server`에 링크되지 않는다.** `fork` + `execve`로 런타임에 별도 프로세스로 실행된다. 그래서 `crash`가 죽어도 서버는 살아남는다. 프로세스 경계가 격리벽이다.
- **`tests/*.c`는 서버와 다른 바이너리다.** `Makefile:17`을 보면 테스트 지원 소스가 `http_request.c`, `http_response.c`, `config.c`, `util.c` 네 개뿐이다. 즉 `static_file.c`, `cgi.c`, `server.c`는 단위 테스트 대상이 아니고 통합 테스트로만 검증된다.
- **`include/*.h`는 계약서다.** 특히 `http.h`가 요청/응답 구조체와 상태 코드를 정의해 파서·핸들러·응답 빌더가 같은 언어를 쓰게 한다.

### Phase 2에서 의미

- 연결 루프와 연결별 상태(`pending`, `requests`)가 `server.c`에만 있다.
- `http_request_t`는 요청마다 새로 만들어지고 전역 상태가 없다. 유일한 전역은 `g_stop`(시그널)과 `cfg`(읽기 전용)뿐이다.
- 그래서 스레드 풀이든 `poll` 루프든 **핸들러는 그대로 재사용**할 수 있다. 동시성 작업은 사실상 `server.c` 하나에 갇혀 있다.

---

## 3. 시작 흐름

### `main()`

`src/main.c:6` · `int` · 프로그램 진입점

- **`config_t cfg`를 스택에 잡고 `config_init`으로 기본값을 채운다.** 초기화하지 않은 스택 쓰레기값이 서버 설정으로 쓰이는 사고를 막는다.
- **`config_parse_cli`로 CLI를 반영한다.** 실패하면 usage를 stderr에 찍고 `1`로 종료한다.
- **`config_validate`로 환경을 검사한다.** docroot/cgi_dir가 실제 디렉터리인지 확인하고, 실패하면 역시 종료한다.
- **`server_run`만 실제로 서버를 띄운다.** 반환값이 `0`이면 `0`, 아니면 `1`을 프로세스 종료 코드로 돌려준다.
- 즉 `main`은 **"설정 → 검증 → 구동" 세 단계를 순서대로 엮는 역할**만 하고, 로직은 각 모듈에 넘긴다.

### `config_init()`

`src/config.c:10` · `void` · ← `main()`

- **`memset(cfg, 0, sizeof *cfg)`로 구조체 전체를 0으로 민다.** 이게 없으면 `cgi_dir` 같은 char 배열에 임의의 바이트가 남아 엉뚱한 경로가 될 수 있다.
- **포트 기본값 8080, docroot 기본값 `./www`, cgi_dir는 빈 문자열**로 둔다. 빈 cgi_dir는 "CGI 기능 꺼짐"을 뜻한다.
- **D11 한계값을 넣는다.** URI 4096, 헤더 16384바이트, 헤더 100개, 본문 8MB.
- **타임아웃/keep-alive 기본값을 넣는다.** 요청 20초, idle 10초, 최대 100요청.
- 파일 시스템도, 소켓도 건드리지 않는다. **순수하게 메모리만 채우는 함수**다.

### `copy_path()`

`src/config.c:25` · `static int` · ← `config_parse_cli()`

- **`snprintf`로 경로를 고정 크기 버퍼에 복사한다.** `strcpy`를 쓰면 긴 인자 하나로 스택 버퍼가 넘친다.
- **`snprintf`의 반환값이 버퍼 크기 이상이면 `-1`.** 잘린 경로를 조용히 받아들이지 않는다. 잘린 docroot는 엉뚱한 디렉터리를 서빙하는 결과로 이어질 수 있다.
- 성공 시 `0`.

### `parse_int_arg()`

`src/config.c:30` · `static int` · ← `config_parse_cli()`

- **`strtol`로 정수를 파싱하고, `end == s`로 "숫자가 아예 없음"을 잡는다.**
- **`*end != '\0'`로 뒤에 붙은 쓰레기를 잡는다.** `"8080abc"`를 8080으로 받아들이지 않는다.
- **`errno`로 오버플로를 잡고, `min`/`max` 범위를 검사한다.**
- `atoi`를 쓰지 않은 이유가 이 세 가지다. `atoi`는 실패를 알려주지 않는다.

### `parse_size_arg()`

`src/config.c:40` · `static int` · ← `config_parse_cli()`

- **`strtoull`로 크기를 파싱한다.** 음수가 들어오면 `strtoull`이 감싸서 거대한 값이 되므로, `-1` 같은 입력은 범위 검사에서 걸린다.
- **0을 거부한다.** "헤더 0바이트", "본문 0바이트" 같은 한계는 서버를 사실상 마비시킨다.
- **`max`를 넘으면 `-1`.** `--max-uri`, `--max-header`는 기본값이 상한이라 **키울 수 없고 줄일 수만 있다.** `--max-body`만 1GiB까지 올릴 수 있다.

### `config_parse_cli()`

`src/config.c:50` · `int` · ← `main()`

- **`argc < 3`이면 실패.** 포트와 docroot는 필수다.
- **`argv[1]`은 포트(1~65535), `argv[2]`는 docroot.** 포트는 임시 변수에 받아 성공했을 때만 `cfg`에 쓴다.
- **`cfg->cgi_dir[0] = '\0'`로 비운다.** "CLI에 cgi_dir가 있으면 그것만, 없으면 빈 값"이라는 규칙을 여기서 확정한다.
- **`argv[3]`부터 루프를 돌며 `--timeout`, `--max-uri`, `--max-header`, `--max-body`, `--keepalive-idle`, `--keepalive-max`를 처리한다.**
- **값은 `i++`로 건너뛰며 읽는다.** `i >= argc` 검사로 "값 없이 플래그만 있는 경우"를 잡는다.
- **플래그가 아닌 첫 토큰은 cgi_dir로 흡수한다.** 그래서 `./server 8080 ./www --timeout 5 ./cgi-bin`처럼 순서를 섞어도 된다.
- **미인식 토큰이 두 개 이상이면 실패.** 오타 난 플래그를 조용히 무시하지 않으려는 것이다.
- 실패 시 `-1`, 성공 시 `0`. **어느 인자가 틀렸는지는 알려주지 않는다** (현재 한계).

### `config_validate()`

`src/config.c:110` · `int` · ← `main()`

- **포트 범위를 한 번 더 검사한다.** `config_parse_cli`가 이미 했지만, 다른 경로로 `config_t`가 만들어질 수 있어 방어적으로 남긴다.
- **`stat(cfg->docroot)`로 존재를 확인하고 `S_ISDIR`로 디렉터리인지 본다.** 파일을 docroot로 주면 실패한다.
- **`cgi_dir`가 비어 있지 않을 때만 같은 검사를 한다.** 빈 값은 "CGI 없음"이라는 정상 상태다.
- **실패하면 어떤 경로가 왜 문제인지 stderr에 찍는다.** 여기서만 사용자에게 친절한 에러가 나온다.
- **소켓을 만들기 전에 실패하므로 정리할 자원이 없다.** 이게 "설정 단계와 구동 단계를 분리"한 실질적 이득이다.

---

## 4. 연결 수락 흐름

### `handle_signal()`

`src/server.c:21` · `static void` · ← 시그널 수신 시

- **첫 시그널은 `g_stop = 1`만 설정한다.** 시그널 핸들러 안에서는 `printf`, `malloc`, `close` 같은 함수를 부르면 안 된다(async-signal-safe가 아니다).
- **두 번째 시그널은 `_exit(1)`로 즉시 종료한다.** `_exit`는 async-signal-safe라 핸들러에서 안전하게 쓸 수 있다. 정리 없이 빠지는 강제 종료다.
- **`volatile sig_atomic_t`라 컴파일러가 최적화로 접근을 없애지 않는다.** 메인 루프가 이 값을 폴링한다.
- 첫 시그널의 실제 종료는 핸들러가 아니라 `server_run`/`handle_connection`이 한다.

### `setup_signals()`

`src/server.c:28` · `static int` · ← `server_run()`

- **SIGPIPE 무시**: 클라이언트가 응답 도중 끊겨도 서버가 죽지 않게 한다. `writen`이 `EPIPE`를 반환하고 호출자가 정리한다.
- **SIGINT/SIGTERM 핸들러**: 첫 시그널은 `g_stop = 1`, 두 번째 시그널은 `_exit(1)`로 강제 종료한다.
- **`sigaction`에 `SA_RESTART`를 주지 않아서 `accept`/`read`가 `EINTR`로 깰 수 있게 한다.** 이게 없으면 시그널을 받아도 `accept`가 재시작되어 루프가 `g_stop`을 확인하지 못할 수 있다.
- 실패 시 `-1`, 성공 시 `0`.

### `create_listen_socket()`

`src/server.c:45` · `static int` · ← `server_run()`

- **`socket(AF_INET, SOCK_STREAM, 0)`으로 TCP 소켓을 만든다.** IPv4 스트림 소켓.
- **`FD_CLOEXEC`를 설정한다.** CGI 자식 프로세스가 `execve` 후에도 listenfd를 들고 있으면, 서버를 재시작해도 포트가 붙잡혀 있을 수 있다.
- **`SO_REUSEADDR`를 켠다.** 서버를 껐다 켤 때 `TIME_WAIT` 상태의 연결 때문에 `bind`가 실패하는 것을 완화한다.
- **`INADDR_ANY`에 바인딩한다.** 모든 인터페이스에서 접속을 받는다는 뜻이다.
- **`listen(SOMAXCONN)`으로 백로그를 커널 최대치로 둔다.** 순간적으로 몰리는 연결을 커널 큐에 쌓아둔다.
- 어느 단계든 실패하면 **열었던 fd를 닫고 `-1`을 반환한다.** 부분 실패에서 fd를 새지 않게 하는 패턴이다.

### `server_run()`

`src/server.c:230` · `int` · ← `main()`

- **시그널을 먼저 등록하고 listen 소켓을 만든다.** 순서가 중요하다. 소켓을 만든 뒤 시그널을 등록하면 그 사이에 Ctrl-C가 들어와 기본 동작으로 죽을 수 있다.
- **`while (!g_stop)`으로 `accept`를 반복한다.** Phase 1은 단일 스레드라 한 번에 연결 하나만 처리한다.
- **종료 시 `handle_connection`은 진행 상태에 따라 중단하거나 응답을 마무리하고, `listenfd`는 그 뒤에 닫힌다.** 헤더/본문을 읽는 중이면 중단하고, 이미 파싱된 요청의 응답은 끝까지 보낸다. 그 사이 backlog에 들어온 연결은 RST될 수 있다 (Phase 2에서 개선).
- **`EINTR`/`ECONNABORTED`는 `continue`.** 시그널이나 클라이언트의 연결 철회는 정상적인 사건이다.
- **`EMFILE`이면 로그를 남기고 1초 쉰다.** fd가 고갈된 상태에서 바로 재시도하면 로그 폭풍과 CPU 낭비가 된다. 1초 쉬면 다른 연결이 정리될 시간이 생긴다.
- **그 외 에러는 `break`.** listen 소켓 자체가 망가진 상황이라 루프를 돌 수 없다.
- **`handle_connection`이 끝나면 반드시 `close(connfd)`.** keep-alive 루프가 내부에서 끝나든 에러로 끝나든 호출자 책임으로 통일했다.
- 루프를 빠져나오면 **`listenfd`를 닫고 종료 로그를 남긴다.**

### `server_request_stop()`

`src/server.c:268` · `void` · 현재 미사용 (테스트/확장용 API)

- **외부에서 `g_stop`을 세울 수 있게 열어둔 함수다.** 지금은 시그널 핸들러가 직접 세우므로 호출되는 곳이 없다.
- Phase 2에서 워커를 정리하거나, 테스트에서 서버를 프로그램적으로 멈출 때 쓸 수 있다.

---

## 5. 연결 처리 흐름 — `handle_connection()`

`src/server.c:111` · `static void` · ← `server_run()`

이 함수가 Phase 1의 심장이다. 한 연결에서 keep-alive 요청을 여러 개 처리한다.

- **`peer_ip`를 문자열로 바꿔 로그와 CGI `REMOTE_ADDR`에 쓴다.** `inet_ntop` 실패 시 `"?"`.
- **`set_send_timeout`으로 `SO_SNDTIMEO`을 건다.** 클라이언트가 응답을 안 읽어 송신 버퍼가 차도 `write`가 최대 `request_timeout_sec`까지만 기다린다.
- **`char buf[16384]` 하나를 연결 내내 재사용한다.** 요청마다 malloc하지 않는다.
- **`max_len`은 `cfg->max_header_bytes`와 버퍼 크기 중 작은 값.** CLI로 헤더 한계를 키워도 물리 버퍼를 넘을 수 없다.
- **`pending`은 이전 요청 뒤에 남은 바이트 수.** 파이프라인/keep-alive에서 다음 요청이 이미 버퍼에 들어와 있을 수 있다.
- **`requests`는 이 연결에서 처리한 요청 수.** `keepalive_max_requests`와 비교한다.

### 헤더 읽기 루프

- **`len = pending`으로 시작하고 `pending = 0`으로 비운다.** 남은 바이트를 이번 요청의 시작으로 삼는다.
- **`has_header_end`로 `\r\n\r\n`을 찾을 때까지 읽는다.** 헤더 끝을 알아야 본문 시작 위치를 계산할 수 있다.
- **타임아웃을 상황에 따라 고른다.**
  - 첫 요청 대기(`len == 0 && requests == 0`) → `request_timeout_sec`
  - keep-alive로 다음 요청 대기(`len == 0 && requests > 0`) → `keepalive_idle_sec`
  - 요청이 일부 들어온 상태(`len > 0`) → 다시 `request_timeout_sec`
- **`EINTR`이면 `g_stop`을 확인한다.** 종료 중이면 바로 return하고, 아니면 재시도한다. 시그널로 `read`가 깨진 것은 에러가 아니다.
- **`EAGAIN`/`EWOULDBLOCK`은 `SO_RCVTIMEO` 만료다.**
  - idle 대기 중이었으면 **응답 없이 조용히 close.** 클라이언트가 그냥 떠난 것과 같다.
  - 요청 도중이었으면 **`408 Request Timeout`.**
- **`read == 0`은 클라이언트가 닫은 것.** 그대로 return.
- **헤더가 `max_len`까지 안 끝나면**: 버퍼가 가득 찼으면 `431`, 아니면 `400`. "너무 큼"과 "형식이 깨짐"을 구분한다.

### `has_header_end()`

`src/server.c:82` · `static int` · ← `handle_connection()`

- **`buf[0..len)`에서 `\r\n\r\n` 4바이트를 찾는다.** 찾으면 `1`, 없으면 `0`.
- **매 호출마다 처음부터 다시 훑는다.** "어디까지 스캔했는지"를 기억하지 않는 상태 없는 함수다.
- **`strstr`/`memmem`을 쓰지 않는 이유**: `buf`는 NUL-terminated가 아니고(본문에 `\0`이 들어올 수 있다), `memmem`은 POSIX 표준이 아니다. 길이를 아는 수동 루프가 가장 안전하고 이식성이 좋다.
- **비용**: 호출 한 번이 `O(len)`. 요청이 `k`번의 `read`로 도착하면 `O(k · n)`, 최악(1바이트씩 도착)이면 `O(n²)`. `n`은 최대 16KB라 실용 범위다.
- **최적화 아이디어**: 새로 읽은 바이트가 경계를 완성할 수 있는 겹침은 **최대 3바이트**다. 이전 길이에서 3을 뺀 지점부터 스캔하면 전체 `O(n)`이 된다.

```c
/* 개념 코드: 이전 len에서 3바이트 겹쳐서 스캔 */
size_t start = (old_len >= 3) ? old_len - 3 : 0;
for (size_t i = start; i + 3 < len; i++) {
    if (buf[i] == '\r' && buf[i+1] == '\n' &&
        buf[i+2] == '\r' && buf[i+3] == '\n')
        return 1;
}
```

- **채택하지 않은 이유**: `old_len`(또는 `scan_pos`)을 호출자와 공유해야 해서 시그니처와 상태 관리가 복잡해진다. 16KB 상한에서는 이득이 측정되지 않는다.
- **레이어 간 재스캔**: `has_header_end`가 `\r\n\r\n`을 찾은 뒤, `parse_headers`가 다시 처음부터 줄 단위로 `\r\n`을 훑고, `parse_uri`가 경로를 또 훑는다. 각 계층이 자기 역할만 하도록 **의도적으로 여러 번 훑는 구조**다.
- **문제가 되는 시점**: `max_len`을 크게 올릴 때, 1바이트씩 천천히 보내는 slowloris류가 많을 때, Phase 2에서 동시 연결 수가 늘어날 때. 그때 `scan_pos` 기반 버전이 첫 번째 최적화 후보가 된다.

### 파싱과 본문

- **`http_request_init`으로 요청 구조체를 0으로 초기화한다.** 이전 요청의 값이 남으면 안 된다.
- **`http_request_parse_buffer`가 `HTTP_PARSE_INCOMPLETE`를 돌려주면 `400`으로 바꾼다.** 헤더 끝을 확인하고 들어왔으므로 실제로는 나오면 안 되는 값이다.
- **파싱 성공 시에만 `http_request_read_body`를 부른다.** 잘못된 요청의 본문을 읽을 이유가 없다.
- **에러면 `http_response_send_error` 후 `close`.** 에러 응답은 항상 연결을 닫는다. 남은 바이트를 해석할 신뢰가 없기 때문이다.

### keep-alive 판단

- **`requests++`를 먼저 한다.** "이번 요청까지 포함해서 몇 개인가"를 세야 최대 요청 수를 정확히 자를 수 있다.
- **`keep = req.keep_alive && requests < keepalive_max_requests && !g_stop`.**
  - `req.keep_alive`는 파서가 결정한다 (HTTP/1.1 기본 true, HTTP/1.0 기본 false, `Connection` 헤더로 조정).
  - 최대 요청 수에 도달하면 `keep`이 false가 되어 `Connection: close`로 응답한다.
  - 종료 중(`g_stop`)이면 `keep`이 false가 되어, 이번 응답에 `Connection: close`를 붙인다. 클라이언트가 이 연결을 재사용하지 않게 하려는 것이다.
- **`close_after`를 모든 응답 함수에 넘긴다.** 응답 헤더의 `Connection` 값이 이 값으로 결정된다.

### 라우팅

- **`OPTIONS`는 내용과 무관하게 공통 응답.** `Allow` 헤더를 붙인다.
- **`cfg->cgi_dir`가 비어 있지 않고 경로가 `/cgi-bin/`으로 시작하면 CGI.**
- **나머지는 정적 파일.**
- **라우팅 함수의 반환값을 무시한다.** 응답 전송 실패는 이미 소켓이 끊긴 경우라 복구할 방법이 없다.

### 버퍼 잔여 바이트

- **`body_in_buf`는 버퍼에 이미 들어온 본문 바이트 수.** `len - header_len`과 `body_len` 중 작은 값.
- **본문을 fd에서 더 읽은 경우 그 바이트는 버퍼에 없으므로 `consumed`에 포함하지 않는다.**
- **`consumed = header_len + body_in_buf`를 빼고 남은 바이트를 `memmove`로 앞으로 당긴다.** 다음 요청의 시작일 수 있다.
- **`pending = leftover`로 다음 루프에 넘긴다.**

### 마무리

- **`http_request_free`로 본문 malloc을 해제한다.** 헤더는 버퍼를 가리키는 포인터라 해제할 것이 없다.
- **`keep`이 false이거나 `g_stop`이면 return.** 응답 도중에 시그널이 왔더라도 다음 요청은 읽지 않는다. 호출자가 `connfd`를 닫는다.

### 종료 흐름

```text
SIGINT/SIGTERM (첫 번째)
  → g_stop = 1
  → accept/read가 EINTR로 깸
  → 헤더/본문을 읽는 중이면 중단
  → 이미 파싱된 요청이면 응답을 끝까지 전송
  → keep 계산에 !g_stop이 들어가 Connection: close
  → 루프 끝에서 g_stop 확인 후 return
  → server_run이 connfd와 listenfd를 닫고 종료

SIGINT/SIGTERM (두 번째)
  → handle_signal이 _exit(1)로 즉시 종료
```

- **Phase 1 한계**: 요청 처리 중에는 `listenfd`가 열려 있어, backlog에 쌓인 연결은 종료 시 RST될 수 있다. Phase 2에서 accept 담당 주체가 `listenfd`를 먼저 닫는 구조로 해결한다.

---

## 6. 읽으면서 주의할 점

이 문서를 코드와 대조할 때 알아두면 좋은 현재 구현의 특성이다.

- **에러 응답은 거의 항상 연결을 닫는다.** `http_response_send_error(..., 1)`이 대부분이다. 파싱 실패 후 남은 바이트를 신뢰할 수 없기 때문이다.
- **응답 상태 라인은 항상 `HTTP/1.1`이다.** HTTP/1.0 요청에도 1.1로 응답한다.
- **`SO_RCVTIMEO`와 `SO_SNDTIMEO`을 모두 설정한다.** 읽기와 쓰기 모두 `request_timeout_sec`으로 bounded된다. `writen`은 타임아웃 시 `-1`을 반환하고 호출자가 연결을 닫는다.
- **Phase 1 종료 시 backlog 연결은 RST될 수 있다.** 단일 스레드라 요청 처리 중 `listenfd`를 즉시 닫을 수 없어서다. Phase 2에서 accept 담당 주체가 먼저 `listenfd`를 닫는 구조로 해결한다.
- **`has_header_end`는 매 read마다 버퍼 전체를 다시 훑는다.** 최악 O(n²)이지만 16KB 상한이라 실용 범위다. 비용 분석과 최적화 방향은 5절 `has_header_end()` 참고.
- **CGI 타임아웃은 idle 기준이다.** 전체 실행 시간 제한이 아니다.
- **`ci_contains`는 substring 검색이다.** 정식 토큰 파서가 아니다.
- **`http_response_send_memory`는 keep-alive를 지원하지 않는다.** 테스트 전용 경로다.
- **`server_request_stop`, `http_request_reset`, `http_response_set_status`는 현재 미사용이다.** Phase 2/테스트를 위한 여지다.

---

## 더 찾아보기

- 함수별 상세: [function_reference.md](function_reference.md)
- 모듈 구조: [architecture.md](architecture.md)
