# CGI — 요청이 처리되는 방식

이 문서는 `GET /cgi-bin/echo`가 들어왔을 때 서버 안에서 무슨 일이 일어나는지, 그리고 CGI가 정확히 무엇인지 설명한다.
`docs/function_reference.md`의 4절(CGI 흐름)을 처음 읽는 사람 기준으로 풀어 쓴 버전이다.

---

## 1. 한 줄 정의

> **`GET /cgi-bin/echo`를 보내면, 서버는 `cgi-bin/echo` 파일을 읽어서 보내는 게 아니라 그 프로그램을 실행해서, 프로그램이 뱉은 출력을 응답으로 보낸다.**

파일 내용이 응답이 아니라 **프로그램의 출력이 응답**이다. 이게 CGI의 전부다.

---

## 2. 정적 파일과 비교

### `GET /index.html`

```text
서버: www/index.html 파일을 연다
     → 파일 내용을 그대로 읽는다
     → 그 바이트를 응답 본문으로 보낸다
```

파일 내용이 곧 응답이다.

### `GET /cgi-bin/echo`

```text
서버: cgi-bin/echo 프로그램을 실행한다 (fork + execve)
     → 프로그램의 stdout을 읽는다
     → 그 출력을 응답으로 보낸다
```

프로그램의 출력이 응답이다.

`cgi-bin/echo`는 컴파일된 실행 파일(바이너리)이다. 그 파일을 그대로 읽어서 보내면 글자가 깨진 쓰레기가 나온다. 그래서 **실행**한다.

---

## 3. `GET /cgi-bin/echo?x=1`의 실제 흐름

### 3.1 클라이언트가 보냄

```http
GET /cgi-bin/echo?x=1 HTTP/1.1
Host: 127.0.0.1:8080

```

### 3.2 서버가 파싱

```text
method = GET
path   = /cgi-bin/echo
query  = x=1
```

`parse_uri`가 `?`를 기준으로 path와 query를 나눈다. query는 디코딩하지 않고 원본 그대로 보존한다.

### 3.3 라우팅

`src/server.c`의 `handle_connection`:

```c
} else if (cfg->cgi_dir[0] != '\0' &&
           strncmp(req.path, "/cgi-bin/", 9) == 0) {
    cgi_handle(connfd, &req, cfg, peer_ip, close_after);
}
```

두 조건이 모두 참이어야 CGI로 간다.

| 조건 | 뜻 |
|---|---|
| `cfg->cgi_dir[0] != '\0'` | CGI 기능이 켜져 있다 (`argv[3]`을 줬다) |
| `strncmp(req.path, "/cgi-bin/", 9) == 0` | URL이 `/cgi-bin/`으로 시작한다 |

조건 평가 자체는 메모리 비교만 한다. 파일 시스템은 `cgi_handle` 안에서 처음 건드린다.

### 3.4 서버가 프로그램을 실행

```text
/cgi-bin/echo  →  ./cgi-bin/echo 파일
                   realpath로 cgi_dir 안인지 확인
                   access(X_OK)로 실행 가능 확인
                   pipe() 생성
                   fork()
                   자식이 execve("./cgi-bin/echo")
```

### 3.5 프로그램이 받는 것

프로그램은 HTTP를 모른다. **환경변수와 stdin**으로만 정보를 받는다.

```text
REQUEST_METHOD  = GET
QUERY_STRING    = x=1
CONTENT_LENGTH  = 0
SCRIPT_NAME     = /cgi-bin/echo
SERVER_PROTOCOL = HTTP/1.1
REMOTE_ADDR     = 127.0.0.1
stdin           = (본문 없음)
```

`build_cgi_env`가 이 값들을 채운다. `SCRIPT_NAME`은 URL 경로, `REMOTE_ADDR`은 접속한 클라이언트 IP다.

### 3.6 프로그램이 하는 일

`cgi-bin/echo.c`:

```c
int main(void) {
    const char *query = getenv("QUERY_STRING");
    const char *method = getenv("REQUEST_METHOD");
    ...
    printf("Content-Type: text/plain\r\n\r\n");
    printf("METHOD=%s\n", method ? method : "");
    printf("QUERY=%s\n", query ? query : "");
    ...
}
```

환경변수를 읽어서 `printf`로 출력하는 게 전부다. 소켓도 모르고, HTTP도 모른다.

### 3.7 프로그램이 뱉는 출력

```text
Content-Type: text/plain

METHOD=GET
QUERY=x=1
CONTENT_TYPE=
CONTENT_LENGTH=0
SCRIPT_NAME=/cgi-bin/echo
SERVER_PROTOCOL=HTTP/1.1
REMOTE_ADDR=127.0.0.1
BODY=
```

이게 **stdout**으로 나온다. 서버는 `pipe`의 읽기 끝으로 이걸 받는다.

### 3.8 서버가 HTTP 응답으로 변환

`cgi.c`의 `send_cgi_response`가:

- `\r\n\r\n` 기준으로 헤더/본문을 가른다
- `Content-Type: text/plain`을 꺼낸다
- **`Content-Length`는 서버가 직접 계산해서 붙인다**
- `Connection`도 서버가 붙인다
- `Status:` 헤더가 있으면 HTTP 상태로 사용한다 (없으면 200)

즉 CGI가 `Content-Length`를 틀리게 써도 클라이언트는 깨지지 않는다. 최종 책임은 서버가 진다.

### 3.9 클라이언트가 보는 것

```http
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: <서버가 계산>
Connection: keep-alive

METHOD=GET
QUERY=x=1
CONTENT_TYPE=
CONTENT_LENGTH=0
SCRIPT_NAME=/cgi-bin/echo
SERVER_PROTOCOL=HTTP/1.1
REMOTE_ADDR=127.0.0.1
BODY=
```

평범한 HTTP 응답이다. 클라이언트 입장에서는 정적 파일이든 CGI든 구분이 안 간다.

---

## 4. CGI의 계약

CGI는 라이브러리도 언어도 아니다. **서버와 프로그램 사이의 약속**이다.

| 통로 | 방향 | 내용 |
|---|---|---|
| 환경변수 | 서버 → 프로그램 | `REQUEST_METHOD`, `QUERY_STRING`, `CONTENT_TYPE`, `CONTENT_LENGTH`, `SCRIPT_NAME`, `SERVER_PROTOCOL`, `SERVER_PORT`, `REMOTE_ADDR` |
| stdin | 서버 → 프로그램 | 요청 본문 (POST 데이터) |
| stdout | 프로그램 → 서버 | 응답 헤더 + 빈 줄 + 응답 본문 |

> 서버는 CGI에 `PATH=/usr/bin:/bin`만 넘기고 나머지 환경은 물려주지 않는다. `#!/usr/bin/env python3` 같은 스크립트는 `env`가 `PATH`에서 interpreter를 찾아야 하므로, C CGI 바이너리나 명시적 interpreter 경로를 사용한다.

프로그램 입장에서는 `getenv()`로 요청을 읽고 `printf()`로 응답을 쓰면 끝이다. HTTP 소켓을 몰라도 되고 `accept()`도 몰라도 된다.

### POST는 어떻게 받나

```text
서버: req.body를 tmpfile()에 쓴다
     → 자식의 stdin을 그 파일로 dup2
     → CONTENT_LENGTH에 본문 길이를 넣는다
프로그램: getchar()/read(0, ...)로 본문을 읽는다
```

파이프 대신 임시 파일을 쓰는 이유는 **교착(deadlock) 방지**다. 부모가 본문을 쓰면서 자식 출력을 동시에 읽지 않으면 서로 기다리다 멈출 수 있다.

---

## 5. "CGI"라는 말은 두 가지로 쓰인다

| 표현 | 뜻 |
|---|---|
| **CGI** (규약) | 서버와 프로그램이 대화하는 **약속** (환경변수 + stdin + stdout) |
| **CGI 프로그램** | 그 약속을 따르는 **실행 파일** (`cgi-bin/echo`) |

"CGI가 뭐 하는 애야?"라고 물으면 — 정확히는 CGI는 **규약**이라 스스로 하는 게 없다. `echo` 같은 **프로그램이 일을 한다.** 사람들이 편하게 "CGI 실행한다"고 말하는 것이다.

---

## 6. 비유: 식당

```text
손님    = 클라이언트
홀 직원 = 웹서버
주방    = CGI 프로그램

손님: "에코 하나 주세요"          (HTTP 요청)
홀:   주문서에 적는다             (환경변수)
      주방에 넘긴다               (fork/execve, stdin)
주방: 요리한다                    (프로그램 실행)
      접시에 담아 내놓는다        (stdout)
홀:   접시를 손님에게 가져간다    (HTTP 응답)
```

주방은 손님을 직접 보지 않는다. 홀 직원이 주문서와 접시를 중계한다. 그게 CGI다.

---

## 7. 왜 굳이 프로그램을 실행하나

`/echo`는 장난감이지만 원리는 같다.

| 요청 | 정적 파일로 가능? | CGI로 가능? |
|---|---|---|
| `/index.html` | ✅ | (할 필요 없음) |
| `/search?q=hello` | ❌ 검색 결과가 파일로 없음 | ✅ 계산해서 출력 |
| `/time` | ❌ 시간이 계속 바뀜 | ✅ 지금 시각 출력 |
| `POST /login` | ❌ 본문을 검사해야 함 | ✅ stdin으로 받아 처리 |

**응답이 파일로 미리 존재할 수 없을 때** 프로그램을 실행한다.

---

## 8. 코드에서의 위치

| 파일 | 역할 |
|---|---|
| `src/server.c` | `/cgi-bin/` 라우팅, `cgi_dir` 기능 스위치 |
| `src/cgi/cgi.c` `cgi_handle` | 경로 검증, `pipe`/`tmpfile`/`fork`/`execve`, `poll` 타임아웃 |
| `src/cgi/cgi.c` `build_cgi_env` | 환경변수 배열을 부모에서 구성 |
| `src/cgi/cgi.c` `send_cgi_response` | CGI 출력 → HTTP 응답 변환 |
| `cgi-bin/echo.c` | 예제 CGI 프로그램 |

### 한계 (Phase 1)

- **출력이 없거나 비정상 종료(exit≠0, 시그널)면 `500`.** 정상 출력 + exit 0만 200이다. 타임아웃은 `504`.
- **`Status`는 3자리 숫자 + 200~599만.** 중복 `Status`/`Content-Type`, `204/205/304`의 body는 `500`으로 거부한다.
- **204/304는 `Content-Length`와 기본 `Content-Type`을 생략하고, 205는 `Content-Length: 0`만 붙인다.**
- **`201 Created` 같은 reason phrase를 사용한다.** 모르는 상태는 `Unknown` 대신 표준 reason을 추가한다.
- **출력 상한 1MB.** 넘으면 `500`.
- **타임아웃은 CGI 시작 시점부터의 전체 실행 시간 제한**이다. `poll`을 다시 호출해도 deadline이 연장되지 않는다.
- **`Transfer-Encoding: chunked` 요청은 `501`.** Phase 3 프록시에서 다룬다.
- **CGI 프로그램은 `cgi_dir` 밖에 둔다.** docroot 안에 두면 실행 파일이 정적으로 노출될 수 있다.
- **환경변수는 `PATH=/usr/bin:/bin`만 전달한다.** C CGI는 문제없지만 `#!/usr/bin/env python3` 같은 스크립트는 macOS의 `/usr/local/bin`/`/opt/homebrew/bin`이 없어 실패할 수 있다. 현재 지원 범위는 C CGI다.
- **실행 권한은 mode bit로 검사한다.** 서버 사용자 기준 effective `X_OK`와 다를 수 있어 `execve` 실패 시 `500`이 될 수 있다. Linux는 `fexecve`로 fd 실행, macOS는 `execve` fallback이라 최종 파일 교체 race가 남는다. Phase 2에서 `posix_spawn`/`faccessat`으로 정리한다.

---

## 9. 흔한 오해

| 오해 | 사실 |
|---|---|
| "`/cgi-bin/echo`는 파일 다운로드다" | 실행 파일을 실행해서 출력을 보낸다 |
| "CGI는 언어다" | 규약이다. C, 셸, 파이썬 다 가능하다 |
| "서버가 CGI 출력을 그대로 보낸다" | 헤더를 파싱하고 `Content-Length`를 다시 계산한다 |
| "CGI가 HTTP를 안다" | 모른다. 환경변수와 stdin/stdout만 안다 |
| "`argv[3]`은 URL 경로다" | 파일 시스템 위치다. URL은 `/cgi-bin/`으로 고정 |

---

## 10. 정리

> **`GET /cgi-bin/echo` = "echo 프로그램을 실행해서, 그 stdout을 HTTP 응답으로 만들어줘."**

- 정적 파일은 **저장된 바이트**를 보낸다.
- CGI는 **실행 결과**를 보낸다.
- 둘 다 클라이언트에게는 평범한 HTTP 응답으로 보인다.
