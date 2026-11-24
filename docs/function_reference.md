# 함수 레퍼런스 — Phase 1

`function_flow.md`가 **서버가 어떻게 돌아가는지 읽는 가이드**라면, 이 문서는 **함수 하나하나를 찾아보는 사전**이다.
각 항목은 `파일:라인 · 반환형 · 호출자`로 시작하고, 무엇을 하는지와 왜 그렇게 했는지를 불릿으로 적었다.

- 라인 번호는 작성 시점 기준이다. 코드가 바뀌면 함수 이름으로 찾는 걸 권장한다.
- `static`은 파일 밖으로 노출되지 않는 내부 함수다.
- `←`는 호출자를 뜻한다.
- 서버 구동과 연결 처리(1~5절)는 [function_flow.md](function_flow.md)에 있다.

## 목차

1. 요청 파싱 흐름 — `http_request.c`
2. 응답 생성 흐름 — `http_response.c`
3. 정적 파일 흐름 — `static_file.c`
4. CGI 흐름 — `cgi.c`
5. 공용 유틸 — `util.c`
6. 함수 색인

---

## 1. 요청 파싱 흐름 — `http_request_parse_buffer()`

`src/http_request.c:358` · `http_status_t` · ← `handle_connection()`

- **요청 라인의 `\r\n`을 찾는다.** 없으면 `HTTP_PARSE_INCOMPLETE`.
- **요청 라인 길이가 `max_header_bytes`를 넘으면 `431`.**
- **`parse_request_line`으로 메서드/타깃/버전을 채운다.**
- **`keep_alive` 기본값을 버전으로 정한다.** HTTP/1.1은 true, HTTP/1.0은 false. `Connection` 헤더가 이걸 덮어쓴다.
- **`parse_headers`로 헤더를 채우고 `headers_end`를 받는다.**
- **`header_len = headers_end - buf + 2`.** 본문이 시작하는 오프셋이다.
- **`Transfer-Encoding`이 있으면**: `Content-Length`와 함께 있으면 `400` (요청 밀수 공격 방지), 단독이면 `501` (chunked 미지원).
- **HTTP/1.1인데 `Host`가 없으면 `400`.** RFC 7230의 필수 조건이다.
- **POST인데 `Content-Length`가 없으면 `411`.**
- **마지막에 `parse_uri`로 경로/쿼리를 확정한다.**

### `find_crlf()`

`src/http_request.c:12` · `static const char *` · ← `http_request_parse_buffer`, `parse_headers`

- **`\r\n` 두 바이트를 선형 탐색한다.** 헤더 한 줄의 끝을 찾는 기본 연산이다.
- 끝을 못 찾으면 `NULL`. "아직 데이터가 덜 왔다"와 "형식이 깨졌다"는 호출자가 구분한다.

### `parse_request_line()`

`src/http_request.c:74` · `static http_status_t` · ← `http_request_parse_buffer`

- **첫 번째 공백과 두 번째 공백으로 `method`, `target`, `version`을 자른다.** 공백이 2개 미만이면 `400`.
- **메서드 길이가 0이거나 `HTTP_MAX_METHOD` 이상이면 `400`.**
- **타깃 길이가 0이면 `400`, `HTTP_MAX_URI` 이상이면 `414`.**
- **메서드/타깃을 고정 버퍼에 복사하고 NUL로 끝낸다.**
- **`parse_method` 결과가 `HTTP_UNKNOWN`이면 `501`.** 모르는 메서드는 "잘못된 요청"이 아니라 "지원 안 함"이다.
- **버전 파싱 결과를 그대로 반환한다.** `505`가 여기서 나온다.

### `parse_method()`

`src/http_request.c:46` · `static http_method_t` · ← `parse_request_line`

- **`GET`/`POST`/`HEAD`/`OPTIONS`만 정확히 일치하면 인정한다.** 대소문자를 구분한다 (HTTP 메서드는 대소문자 구분).
- 나머지는 `HTTP_UNKNOWN` → 호출자가 `501`로 변환.

### `parse_version()`

`src/http_request.c:58` · `static http_status_t` · ← `parse_request_line`

- **정확히 8자 `HTTP/d.d` 형식만 받는다.** `HTTP/1.1 `처럼 뒤에 공백이 붙으면 `400`.
- **주 버전이 1이 아니거나 부 버전이 0/1이 아니면 `505`.** `HTTP/2.0`은 "형식은 맞지만 지원 안 함"이다.
- 형식 자체가 깨졌으면 `400`. **`400`과 `505`를 구분하는 게 포인트다.**

### `hexval()`

`src/http_request.c:113` · `static int` · ← `decode_uri`

- **`0-9`, `a-f`, `A-F`를 0~15로 바꾼다.** 그 외는 `-1`.
- 퍼센트 디코딩의 최소 단위다.

### `decode_uri()`

`src/http_request.c:123` · `static int` · ← `parse_uri`

- **`%XX`를 원래 바이트로 되돌린다.** `%2e%2e` → `..` 같은 우회를 정규화 전에 펼치기 위해서다.
- **`%` 뒤에 2바이트가 없거나 16진수가 아니면 `-1`.** 잘린 인코딩을 통과시키지 않는다.
- **디코딩 결과가 `\0`이면 `-1`.** `%00`으로 문자열을 자르는 공격을 막는다.
- **출력 버퍼 크기를 매 바이트 검사한다.** 디코딩은 길이를 줄이거나 같게만 하지만, 방어적으로 확인한다.

### `normalize_path()`

`src/http_request.c:152` · `static int` · ← `parse_uri`

- **`strtok_r`로 `/` 기준 세그먼트를 돈다.** `strtok`이 아닌 이유는 재진입 안전성이다.
- **`.` 세그먼트는 버린다.**
- **`..`은 스택에서 하나 pop한다.** 스택이 비었는데 `..`이면 **docroot 탈출 시도**라 `-1` → 호출자가 `403`.
- **일반 세그먼트는 스택에 쌓는다.** 최대 256개(`MAX_PATH_SEGMENTS`).
- **다시 `/`로 이어 붙인다.** 항상 `/`로 시작한다.
- **원래 경로가 `/`로 끝났으면 trailing slash를 보존한다.** `/dir`과 `/dir/`은 다른 의미(301 vs index)라 뺄 수 없다.

### `parse_uri()`

`src/http_request.c:205` · `static http_status_t` · ← `http_request_parse_buffer`

- **타깃 길이를 `cfg->max_uri`와 비교해 `414`를 낸다.** `HTTP_MAX_URI`도 상한으로 함께 건다.
- **`/`로 시작하지 않으면 `400`.** origin-form만 지원한다. `http://host/path` 같은 absolute-form은 프록시(Phase 3)에서 다룰 문제다.
- **`?` 기준으로 path와 query를 나눈다.** query는 **디코딩하지 않고 원본 그대로** 보존한다. CGI가 `QUERY_STRING`으로 받아서 직접 해석하는 게 표준이다.
- **path만 `decode_uri` → `normalize_path`를 거친다.**
- **정규화 실패는 `403`.** "잘못된 요청"이 아니라 "허용되지 않는 경로"다.
- **query 길이가 버퍼를 넘으면 `414`.**

### `parse_content_length()`

`src/http_request.c:239` · `static http_status_t` · ← `parse_headers`

- **값을 32바이트 임시 버퍼에 복사해 NUL-terminate한다.** 헤더 값은 zero-copy 포인터라 끝이 보장되지 않는다.
- **`strtoull`로 파싱하고 `end == tmp`, `*end != '\0'`, `errno`를 모두 검사한다.** `"5x"`, `"-1"`, 오버플로를 전부 거른다.
- **중복 `Content-Length`는 값이 같으면 허용, 다르면 `400`.** 요청 밀수(request smuggling)를 막는 핵심 검사다.
- **`cfg->max_body`를 넘으면 `413`.** 본문을 읽기 전에 거절해서 메모리를 아낀다.

### `trim_ows()`

`src/http_request.c:21` · `static void` · ← `parse_headers`

- **앞뒤의 SP/HTAB만 제거한다.** HTTP의 OWS(optional whitespace) 정의를 따른다.
- **CR/LF는 여기서 다루지 않는다.** 그건 `find_crlf`가 이미 잘라냈다.

### `ci_equal()`

`src/http_request.c:30` · `static int` · ← `parse_headers`

- **길이가 같고 대소문자 무시 비교로 같으면 1.** 헤더 이름은 대소문자를 구분하지 않는다.
- **`strncasecmp`만 쓰면 접두사 일치를 허용하게 되므로 길이 검사를 먼저 한다.** `Content-Length-Extra`가 `Content-Length`로 오인되면 안 된다.

### `ci_contains()`

`src/http_request.c:35` · `static int` · ← `parse_headers`

- **대소문자 무시 부분 문자열 검색.** `Connection: keep-alive, Upgrade` 같은 목록에서 토큰을 찾을 때 쓴다.
- **정식 토큰 파서가 아니라 substring 검색이라는 한계가 있다.** `"enclose"`에 `close`가 들어 있어도 매칭된다. 현재 입력 범위에서는 실용적으로 충분하지만, 엄밀히 하려면 쉼표로 나눠 토큰 단위로 비교해야 한다.

### `parse_headers()`

`src/http_request.c:266` · `static http_status_t` · ← `http_request_parse_buffer`

- **빈 줄(`\r\n`)을 만날 때까지 한 줄씩 돈다.** 그 위치를 `headers_end_out`으로 돌려준다.
- **`p + 1 >= buf_end`면 `HTTP_PARSE_INCOMPLETE`.** 아직 데이터가 덜 왔다.
- **`max_header_bytes` 검사를 두 번 한다.** 현재 위치 기준과 현재 줄 끝 기준. 둘 다 넘으면 `431`.
- **헤더 개수가 `max_headers`를 넘으면 `431`.** 개별 크기가 작아도 개수로 메모리를 고갈시키는 공격을 막는다.
- **콜론이 없으면 `400`.** 헤더 줄의 필수 형식이다.
- **이름은 뒤쪽 OWS만, 값은 앞뒤 OWS를 제거한다.** `Host : x` 같은 표기도 받아들이기 위해서다.
- **이름이 비면 `400`.**
- **zero-copy로 `buf` 안을 가리키는 포인터만 저장한다.** 헤더마다 malloc하면 요청당 수십 번 할당이 생긴다.
- **특별 처리**: `Host`는 `has_host`, `Connection`은 `keep_alive`, `Content-Length`는 `parse_content_length`, `Transfer-Encoding`은 플래그, `Expect: 100-continue`는 `expect_continue`.
- **`Expect`는 CGI body가 필요하고 아직 버퍼에 없을 때 `100 Continue`를 보낸다.** static/OPTIONS는 body를 읽지 않으므로 보내지 않는다.

### `http_request_init()`

`src/http_request.c:342` · `void` · ← `handle_connection`

- **구조체를 0으로 밀고 `method`를 `HTTP_UNKNOWN`으로 둔다.** 0이 `HTTP_GET`이라 그냥 두면 초기화 안 된 요청이 GET으로 보인다.
- 본문 포인터가 `NULL`이 되므로 `http_request_free`가 안전하게 동작한다.

### `http_request_free()`

`src/http_request.c:347` · `void` · ← `handle_connection`

- **`body`만 해제한다.** 헤더는 `buf`를 가리키는 포인터라 해제하면 안 된다.
- **해제 후 `NULL`, `body_len = 0`으로 이중 해제를 막는다.**

### `http_request_reset()`

`src/http_request.c:353` · `void` · 현재 미사용

- **`free` + `init`을 합친 함수.** 연결 루프에서 구조체를 재사용할 때 쓸 수 있게 열어뒀다.
- 지금은 요청마다 `http_request_init`을 새로 부르므로 호출되지 않는다.

### `http_request_read_body()`

`src/http_request.c:472` · `http_status_t` · ← `handle_connection`

- **이전 본문을 먼저 해제한다.** 재사용 시나리오에서 누수를 막는다.
- **`Content-Length`가 없거나 0이면 즉시 `HTTP_OK`.** GET 대부분이 여기서 끝난다.
- **`max_body`를 한 번 더 검사한다.** 파서를 거치지 않은 경로에 대한 방어다.
- **버퍼에 이미 들어온 본문을 먼저 복사한다.** keep-alive에서 다음 요청과 함께 본문이 도착한 경우다.
- **본문은 16KB부터 시작해 두 배씩 `realloc`한다.** `Content-Length` 전체를 한 번에 할당하지 않는다.
- **모자란 만큼만 `readn_deadline`으로 읽는다.** 이미 읽은 바이트를 다시 읽지 않는다.
- **`stop`이 세워지면 본문 읽기를 중단한다.** 시그널로 `read`가 `EINTR`로 깼을 때 `stop`을 확인하고 `-1`을 반환한다. 서버는 이때 에러 응답을 보내지 않고 연결을 닫는다.
- **`EAGAIN`/`EWOULDBLOCK`이면 `408`.** 절대 deadline 만료다.
- **short read면 `400`.** 클라이언트가 본문을 덜 보내고 끊은 경우다.
- **`body_len = content_length`로 확정한다.**

### `http_request_get_header()`

`src/http_request.c:543` · `const char *` · ← `static_file_handle`, `build_cgi_env`

- **헤더 배열을 선형 탐색하며 대소문자 무시 비교한다.** 헤더가 100개 이하라 선형으로 충분하다.
- **값 포인터와 길이를 함께 돌려준다.** zero-copy라 NUL-terminate가 없어서 길이가 필수다.
- **없으면 `NULL`, `value_len = 0`.**

---

## 2. 응답 생성 흐름

### `http_status_reason()`

`src/http_response.c:7` · `const char *` · ← `http_response_send_headers`, `http_response_send_error`

- **상태 코드를 reason phrase로 바꾼다.** `200` → `OK`, `404` → `Not Found`.
- **모르는 코드는 `"Unknown"`.** 새 상태를 추가할 때 여기도 같이 넣어야 한다.
- Phase 1에서 쓰는 코드: 200, 206, 301, 304, 400, 403, 404, 405, 408, 411, 413, 414, 416, 431, 500, 501, 504, 505.

### `http_response_init()`

`src/http_response.c:31` · `void` · ← 모든 응답 함수

- **응답 구조체를 0으로 초기화한다.** `headers_len`이 0이라 헤더 누적이 처음부터 시작된다.

### `http_response_set_status()`

`src/http_response.c:35` · `int` · 현재 미사용

- **상태만 바꾸는 얇은 래퍼.** 지금은 각 함수가 `res.status`를 직접 대입한다.
- API 대칭성을 위해 남겨둔 형태다.

### `http_response_add_header()`

`src/http_response.c:40` · `int` · ← 모든 응답 함수

- **`Name: Value\r\n` 형식으로 4096바이트 버퍼에 이어 붙인다.** 헤더마다 개별 할당을 하지 않는다.
- **남은 공간을 `snprintf`에 넘기고, 잘리면 `-1`.** 조용히 잘린 헤더를 보내면 클라이언트가 응답을 오해한다.
- **공간이 0이면 `-1`.** 헤더가 너무 많은 응답은 서버 버그로 취급한다.

### `http_response_send_headers()`

`src/http_response.c:58` · `int` · ← 모든 응답 함수

- **상태 라인을 만든다.** 항상 `HTTP/1.1`로 보낸다. HTTP/1.0 요청에도 1.1로 응답하는 단순화가 있다 (일반 클라이언트는 허용).
- **누적된 헤더를 그대로 쓴다.**
- **빈 줄 `\r\n`으로 헤더 블록을 끝낸다.**
- **모든 쓰기는 `writen`.** 부분 쓰기와 `EINTR`을 처리한다.

### `http_response_send_redirect()`

`src/http_response.c:80` · `int` · ← `static_file_handle`

- **301 + `Location` + `Content-Length: 0`.** 본문 없는 리다이렉트다.
- **`Connection`을 `close_connection`에 맞춘다.**
- **`/dir` → `/dir/`에만 쓴다.** 쿼리가 있으면 `Location`에 붙여 보존한다.

### `http_response_send_options()`

`src/http_response.c:101` · `int` · ← `handle_connection`

- **200 + `Allow: GET, POST, HEAD, OPTIONS` + `Content-Length: 0`.**
- **리소스 존재 여부와 무관하게 같은 응답을 준다.** "서버가 어떤 메서드를 아는가"를 알려주는 용도다.

### `http_response_send_memory()`

`src/http_response.c:119` · `int` · ← 테스트

- **메모리에 있는 본문을 상태/타입과 함께 보낸다.** 단위 테스트에서 소켓 쌍으로 응답을 검증할 때 쓴다.
- **`Connection: close`가 하드코딩되어 있다.** 범용 함수라 keep-alive 제어가 빠져 있다. 실제 서빙 경로에서는 쓰지 않는다.

### `http_response_send_error()`

`src/http_response.c:220` · `int` · ← 모든 핸들러

- **HTML 에러 본문을 만든다.** `<h1>404 Not Found</h1>` 형태.
- **`Content-Type: text/html; charset=utf-8`, `Content-Length`, `Connection`을 붙인다.**
- **`close_connection`을 인자로 받지만, 대부분의 호출자는 `1`을 넘긴다.** 에러 후에는 남은 바이트를 신뢰할 수 없어 닫는 게 안전하다.
- **본문 버퍼는 512바이트.** 상태 코드와 reason만 들어가므로 충분하다.

---

## 3. 정적 파일 흐름 — `static_file_handle()`

`src/static_file.c:194` · `handler_result_t` · ← `handle_connection`

- **반환값은 `HANDLER_OK`/`HANDLER_CLOSE`/`HANDLER_ERROR`.** 에러 응답(404/403/416 등)은 `HANDLER_CLOSE`라서 연결을 재사용하지 않는다.
- **GET/HEAD가 아니면 `405`.** 정적 파일은 읽기 전용이다.
- **dotfile 세그먼트가 있으면 `404`.** `.env`, `.git` 같은 파일은 존재 자체를 숨긴다 (403이 아니라 404).
- **`realpath(docroot)`로 기준 경로를 확정한다.** symlink가 섞인 docroot도 실제 경로로 비교하기 위해서다.
- **`/`는 `/index.html`로 바꾼다.**
- **`docroot + path`로 파일 경로를 만들고 `stat`한다.** 실패는 `status_from_errno`로 매핑한다.
- **디렉터리 처리**:
  - `/`로 끝나지 않으면 **`301` + `Location: path/`** (쿼리 유지)
  - `/`로 끝나면 **`path + index.html`** 시도
  - index가 없으면 `404`
- **`realpath(filepath)`로 symlink를 해석하고 `path_within_root`로 docroot 안인지 확인한다.** 밖이면 `403`.
- **해석된 경로에도 dotfile 검사를 한다.** `link → .env` 같은 우회를 막는다.
- **`open` → `fstat` → `S_ISREG` 확인.** 디렉터리나 장치 파일을 읽지 않는다.
- **`Last-Modified`를 만들고 `If-Modified-Since`와 비교한다.** `ims >= mtime`이면 **`304`** (본문 없음).
- **`Range`가 있으면 `parse_range_header`로 검증한다.** 유효하면 `206`, 아니면 `416`.
- **헤더**: `Content-Type`(확장자 기반), `Content-Length`, (Range 시) `Content-Range`, `Accept-Ranges: bytes`, `Last-Modified`, `Connection`.
- **GET이면 `send_file`로 본문을 보내고, HEAD면 헤더만 보낸다.**
- **모든 경로에서 `filefd`를 닫는다.** 조기 반환이 많아 close 누락이 생기기 쉬운 구조라 각 분기마다 명시했다.

### `send_file()`

`src/static_file.c:17` · `int` · ← `static_file_handle`

- **64KB 청크로 읽어서 `writen`으로 보낸다.** 파일 전체를 메모리에 올리지 않는다. 2MB 파일도 64KB씩 나간다.
- **`EINTR`은 재시도.** 시그널로 깬 읽기는 실패가 아니다.
- **읽기가 0이면 `-1`.** 파일이 중간에 잘린 경우다. `Content-Length`보다 적게 보냈으므로 연결을 닫는다.
- **쓰기 실패는 `-1`.** 클라이언트가 끊긴 경우다.

### `has_dotfile_segment()`

`src/static_file.c:37` · `static int` · ← `static_file_handle`

- **`/`로 나눈 각 세그먼트가 `.`으로 시작하면 1.** `.`과 `..`도 여기서 걸린다 (이미 정규화됐지만 이중 방어).
- **요청 경로와 symlink 해석 후 경로 둘 다에 적용한다.** 한쪽만 검사하면 symlink로 우회할 수 있다.

### `path_within_root()`

`src/static_file.c:55` · `static int` · ← `static_file_handle`

- **문자열 prefix 비교로 root 안인지 본다.**
- **prefix 다음 문자가 `/` 또는 `\0`인지 확인한다.** 이게 없으면 `/www`가 `/www2`도 통과시킨다.
- `cgi.c`에도 같은 함수가 따로 있다. 두 모듈이 서로 의존하지 않게 복제한 형태다.

### `status_from_errno()`

`src/static_file.c:64` · `static int` · ← `send_error_for_errno`, `static_file_handle`

- **`ENOENT`/`ENOTDIR` → `404`, `EACCES` → `403`, 나머지 → `500`.**
- **"없음"과 "권한 없음"을 구분한다.** 권한 없는 파일은 존재를 알려주는 셈이지만, 403이 404보다 디버깅에 유용하다는 판단이다.

### `send_error_for_errno()`

`src/static_file.c:72` · `static int` · ← `static_file_handle`

- **`status_from_errno` + `http_response_send_error`를 합친다.** `errno`를 보존한 채로 응답해야 해서 래퍼를 둔다.
- **항상 `close_connection = 1`.** 파일 시스템 에러 후에는 연결 상태를 신뢰하지 않는다.

### `parse_size_value()`

`src/static_file.c:81` · `static int` · ← `parse_range_header`

- **Range의 숫자 하나를 파싱한다.** 32바이트 제한, `strtoull`, 엄격한 종료 검사.
- **`parse_size_arg`와 비슷하지만 상한이 없다.** 파일 크기보다 큰 값은 호출자가 clamp한다.

### `parse_range_header()`

`src/static_file.c:99` · `static int` · ← `static_file_handle`

- **`bytes=`로 시작해야 한다.** 아니면 `-1` → `416`.
- **쉼표가 있으면 `-1`.** multipart range는 지원하지 않는다.
- **세 가지 형태를 지원한다.**
  - `bytes=start-end`
  - `bytes=start-` (끝까지)
  - `bytes=-suffix` (마지막 N바이트)
- **suffix가 파일 크기보다 크면 파일 크기로 clamp.**
- **`start >= file_size`면 `-1`.** 시작이 파일 밖이면 만족 불가다.
- **`end`가 파일 크기를 넘으면 `file_size - 1`로 clamp.** RFC가 허용하는 동작이다.
- **`last < start`면 `-1`.** 역순 범위는 거절.

### `send_range_error()`

`src/static_file.c:151` · `static int` · ← `static_file_handle`

- **`416` + `Content-Range: bytes */<size>` + `Accept-Ranges: bytes` + `Content-Length: 0`.**
- **`bytes */size` 형식이 포인트다.** "전체 크기는 이만큼인데 네가 요청한 범위는 만족 불가"라는 뜻이다.

---

## 4. CGI 흐름 — `cgi_handle()`

`src/cgi.c:254` · `handler_result_t` · ← `handle_connection`

- **반환값은 `HANDLER_OK`/`HANDLER_CLOSE`/`HANDLER_ERROR`.** CGI 에러(404/500/504 등)는 `HANDLER_CLOSE`라서 연결을 재사용하지 않는다.
- **`/cgi-bin/` 접두사 확인.** 아니면 `404`. 라우팅에서 이미 걸러지지만 이중 방어다.
- **`name`이 비면 `404`.** `/cgi-bin/` 자체 요청이다.
- **`realpath(cgi_dir)`로 기준 경로를 확정한다.** 실패하면 `500` (설정이 깨진 상태).
- **`cgi_dir/name`으로 스크립트 경로를 만들고 `realpath`로 해석한다.** 실패하면 `404`.
- **`path_within_root`로 cgi_dir 밖 실행을 차단한다.** `../../bin/sh` 같은 탈출을 막는다.
- **`access(X_OK)`로 실행 가능 확인.** 아니면 `404` (존재를 숨긴다).
- **`pipe(out_pipe)`로 자식 stdout 통로를 만든다.**
- **POST 본문은 `tmpfile()`에 쓴다.** 파이프 교착을 피하려는 선택이다. 부모가 본문을 쓰면서 자식 출력을 동시에 읽지 않으면 서로 기다리다 멈출 수 있다.
- **`fork()`** 후 자식/부모로 갈라진다.

### 자식 프로세스

- **stdin을 본문 임시 파일(없으면 `/dev/null`)로 `dup2`.**
- **stdout을 `out_pipe[1]`로 `dup2`.**
- **`out_pipe[0]`, `out_pipe[1]`, `connfd`를 닫는다.** 자식이 클라이언트 소켓을 들고 있으면 연결이 예상보다 늦게 닫힌다.
- **`build_cgi_env`로 요청 정보를 환경변수에 넣는다.**
- **`execve`로 실행하고, 실패하면 `_exit(127)`.** 자식에서 `exit`가 아니라 `_exit`를 쓰는 이유는 부모의 stdio 버퍼를 복사/플러시하지 않기 위해서다.

### 부모 프로세스

- **쓰기 끝(`out_pipe[1]`)과 임시 파일을 닫는다.**
- **출력 버퍼를 `CGI_MAX_OUTPUT + 1`(1MB + NUL)로 잡는다.**
- **`poll`로 자식 출력을 기다린다.** `request_timeout_sec`을 ms로 바꿔 넘긴다.
- **타임아웃이면 `SIGTERM` → `waitpid` → `504 Gateway Timeout`.**
- **출력이 1MB를 넘으면 `500`.** CGI가 무한정 출력하는 것을 막는다.
- **출력이 0바이트면 `500`.** 헤더조차 없는 출력은 정상 응답이 아니다.
- **`waitpid`로 좀비를 거두고**, 정상 출력이면 `send_cgi_response`로 넘긴다.

> **타임아웃의 정확한 의미**: CGI 타임아웃은 **시작 시점부터의 전체 실행 시간 제한**이다. `poll`을 다시 호출해도 deadline이 연장되지 않는다. 19초마다 1바이트씩 출력하는 CGI도 결국 deadline에 걸린다.

### `path_within_root()` (cgi)

`src/cgi.c:22` · `static int` · ← `cgi_handle()`

- **`static_file.c`와 같은 prefix + 경계 검사를 복제했다.** 두 모듈이 서로를 의존하지 않게 하려는 선택이다.
- **`/cgi-bin`과 `/cgi-bin2`를 구분한다.** prefix 다음 문자가 `/` 또는 `\0`인지 확인한다.
- **symlink로 cgi_dir 밖을 가리키는 스크립트를 차단한다.** `realpath` 결과에 적용된다.

### `build_cgi_env()`

`src/cgi.c` · `static char **` · ← `cgi_handle()` (fork 전)

- **`GATEWAY_INTERFACE=CGI/1.1`**: 이 프로세스가 CGI 규약으로 불렸다는 표시.
- **`REQUEST_METHOD`**: `GET`/`POST` 등.
- **`QUERY_STRING`**: `?` 뒤 원본. 디코딩하지 않는다.
- **`CONTENT_LENGTH`**: 본문 길이. `body_len`을 문자열로.
- **`CONTENT_TYPE`**: 요청 헤더에서 복사. 512바이트 이상이면 빈 값.
- **`SCRIPT_NAME`**: `/cgi-bin/echo` 같은 실행 경로.
- **`SERVER_PROTOCOL`**: `HTTP/1.1` 형식.
- **`SERVER_PORT`**: 설정 포트.
- **`REMOTE_ADDR`**: 클라이언트 IP.
- **`PATH=/usr/bin:/bin`만 남기고 서버 환경은 물려주지 않는다.**
- **자식은 `setenv`를 호출하지 않는다.** 부모가 만든 `envp`를 `execve`에 그대로 넘긴다.

### `send_cgi_response()`

`src/cgi.c:39` · `static int` · ← `cgi_handle`

- **`\r\n\r\n` 또는 `\n\n`으로 헤더/본문을 가른다.** CGI 프로그램이 CRLF를 안 쓰는 경우도 받아준다.
- **구분자가 없으면 `500`.** CGI 출력의 최소 형식이다.
- **`output`을 in-place로 자른다.** 어차피 곧 free할 버퍼라 복사하지 않는다.
- **`Status:` 헤더는 HTTP 상태로 변환한다.** 3자리 숫자 200~599만 허용하고, 아니면 `500`. 중복 `Status`/`Content-Type`도 `500`.
- **`Content-Length`와 `Connection`은 버린다.** 서버가 다시 계산한다. CGI가 틀린 길이를 써도 클라이언트가 깨지지 않는다.
- **나머지 헤더는 그대로 전달한다.** `Set-Cookie` 같은 헤더를 살리기 위해서다.
- **`Content-Type`이 없으면 `text/plain`.**
- **`Content-Length`를 실제 본문 길이로 붙이고 `Connection`도 서버 값으로 붙인다.**
- **헤더 전송 후 본문을 `writen`으로 보낸다.**

---

## 5. 공용 유틸 — `util.c`

### `readn_stop()`

`src/util.c:114` · `ssize_t` · ← `readn()`

- **`n`바이트를 채울 때까지 `read`를 반복한다.** `read` 한 번이 요청한 만큼 다 읽는다는 보장이 없다.
- **`stop`이 `NULL`이 아니고 세워져 있으면 중단한다.** `EINTR`로 깼을 때 `stop`을 확인하고 `errno = EINTR`로 `-1`을 반환한다.
- **`stop == NULL`이면 일반 `readn`처럼 동작한다.** 기존 호출자와의 호환을 위한 분기다.
- **반환값은 실제로 읽은 바이트 수.** 짧게 읽었는지는 호출자가 `n`과 비교해서 판단한다.
- **에러 시 `-1`.** `errno`는 그대로 보존된다.

### `readn_deadline()`

`src/util.c` · `ssize_t` · ← `http_request_read_body`

- **절대 deadline까지 `poll`로 기다린 뒤 `read`한다.** 매 read마다 timeout을 새로 주지 않는다.
- **`EAGAIN`/`EWOULDBLOCK`이면 deadline 만료다.**
- **`stop`이 세워지면 `EINTR`로 중단한다.**
- **반환값은 실제로 읽은 바이트 수.** 짧게 읽었는지는 호출자가 `n`과 비교한다.

### `write_body()`

`src/util.c:84` · `ssize_t` · ← 응답 body 전송

- **`writen`으로 body를 보내고, 성공한 바이트를 access log에 누적한다.**
- **헤더 전송은 `writen`을 써서 access log의 body 바이트에 포함하지 않는다.**

### `readn()`

`src/util.c:139` · `ssize_t` · 현재 미사용 (호환용)

- **`readn_stop(fd, buf, n, NULL)`을 부르는 얇은 래퍼다.** 종료 플래그가 필요 없는 곳에서 쓴다.
- 동작은 `readn_stop`과 같다.

### `writen()`

`src/util.c:143` · `ssize_t` · ← 모든 응답 전송

- **`write`가 부분 쓰기를 해도 남은 만큼 계속 쓴다.** 소켓은 요청한 크기만큼 한 번에 쓰지 않을 수 있다.
- **`EINTR` 재시도, `0`이면 중단.**
- **SIGPIPE가 무시되어 있으므로 끊긴 연결은 `-1` + `EPIPE`로 돌아온다.** 프로세스가 죽지 않는다.

### `log_error()`

`src/util.c:60` · `void` · ← 서버 전반

- **`va_list`로 printf 형식을 받아 stderr에 쓰고 개행을 붙인다.**
- **로그 레벨/타임스탬프는 없다.** Phase 1은 최소 로깅만 한다.
- **함수 이름이 `log_error`지만 접속 로그에도 쓴다.** 이름과 용도가 완전히 일치하지는 않는다.

### `mime_type()`

`src/util.c:68` · `const char *` · ← `static_file_handle`

- **마지막 `.` 이후 확장자로 MIME을 정한다.** 대소문자 무시.
- **텍스트 계열은 `charset=utf-8`을 붙인다.** 브라우저의 인코딩 추측을 막는다.
- **모르는 확장자는 `application/octet-stream`.** 브라우저가 실행하지 않고 다운로드하게 한다.
- **파일 내용을 보지 않는다.** 확장자 기반이라 내용과 불일치할 수 있다.

### `days_from_civil()`

`src/util.c:99` · `static int64_t` · ← `timegm_utc`

- **그레고리력 날짜를 1970-01-01 기준 일수로 바꾼다.** Howard Hinnant의 알고리즘이다.
- **`timegm`을 쓰지 않는 이유**: 플랫폼별 제공 여부가 갈리고, `TZ` 환경변수 영향을 받지 않는 구현이 필요했다.
- **음수 연도도 처리한다.** `era` 계산에서 400년 단위로 보정한다.

### `timegm_utc()`

`src/util.c:109` · `static time_t` · ← `http_date_parse`

- **일수 × 86400 + 시/분/초로 epoch 초를 만든다.**
- **로컬 타임존을 전혀 거치지 않는다.** HTTP 날짜는 항상 GMT다.

### `month_index()`

`src/util.c:115` · `static int` · ← `http_date_parse`

- **`Jan`~`Dec`을 1~12로 바꾼다.** 대소문자 무시.
- **모르는 이름은 `-1`.**

### `http_date_format()`

`src/util.c:128` · `int` · ← `static_file_handle`

- **`gmtime_r`로 UTC를 구하고 IMF-fixdate로 포맷한다.** `Fri, 20 Nov 2026 10:47:19 GMT`.
- **`gmtime_r`을 쓰는 이유**: `gmtime`은 정적 버퍼를 돌려줘 스레드 안전하지 않다. Phase 2에서 스레드를 붙일 때를 대비한 선택이다.
- **버퍼가 모자라면 `-1`.** 잘린 날짜를 헤더에 넣으면 캐시 검증이 깨진다.

### `http_date_parse()`

`src/util.c:149` · `int` · ← `static_file_handle`

- **`sscanf`로 `Wdy, DD Mon YYYY HH:MM:SS GMT` 형식만 받는다.** `%n`으로 문자열 끝까지 소비됐는지 확인해 뒤에 쓰레기가 붙은 입력을 거절한다.
- **요일(`%*3s`)은 검증하지 않는다.** 날짜 계산에 필요 없기 때문이다.
- **범위 검사**: 일 1~31, 연도 1970 이상, 시 0~23, 분 0~59, 초 0~60(윤초 허용).
- **월 이름이 틀리면 `-1`.**
- **`timegm_utc`로 epoch 초를 만든다.**
- **RFC 850(`Sunday, 06-Nov-94`)과 asctime 형식은 지원하지 않는다.** 현대 클라이언트는 IMF-fixdate를 쓴다.

---

## 6. 함수 색인

| 함수 | 위치 | 한 줄 역할 |
|---|---|---|
| `main` | `src/main.c:6` | 설정 → 검증 → 구동 |
| `config_init` | `src/config.c:10` | 설정 구조체 기본값 |
| `copy_path` | `src/config.c:25` | 경로 복사 + 절단 검사 |
| `parse_int_arg` | `src/config.c:30` | 엄격한 정수 파싱 |
| `parse_size_arg` | `src/config.c:40` | 엄격한 크기 파싱 |
| `config_parse_cli` | `src/config.c:50` | `argv` 해석 |
| `config_validate` | `src/config.c:110` | docroot/cgi_dir 검증 |
| `handle_signal` | `src/server.c:21` | 첫 시그널 `g_stop`, 두 번째 `_exit` |
| `setup_signals` | `src/server.c:28` | SIGPIPE/SIGINT/SIGTERM 등록 |
| `create_listen_socket` | `src/server.c:45` | socket/bind/listen |
| `has_header_end` | `src/server.c:82` | `\r\n\r\n` 탐색 |
| `set_send_timeout` | `src/server.c` | `SO_SNDTIMEO` 설정 |
| `handle_connection` | `src/server.c` | 연결별 요청 루프 |
| `server_run` | `src/server.c` | accept 루프 |
| `server_request_stop` | `src/server.c` | 외부 종료 API (미사용) |
| `now_ms` | `src/util.c` | monotonic 밀리초 |
| `readn_deadline` | `src/util.c` | deadline 기반 n바이트 읽기 |
| `readn_stop` | `src/util.c` | 종료 플래그 지원 n바이트 읽기 |
| `readn` | `src/util.c:139` | `readn_stop(..., NULL)` 래퍼 |
| `writen` | `src/util.c:143` | n바이트 쓰기 |
| `log_error` | `src/util.c:60` | stderr 로그 |
| `mime_type` | `src/util.c:68` | 확장자 → MIME |
| `days_from_civil` | `src/util.c:99` | 날짜 → epoch 일수 |
| `timegm_utc` | `src/util.c:109` | UTC epoch 초 |
| `month_index` | `src/util.c:115` | 월 이름 → 숫자 |
| `http_date_format` | `src/util.c:128` | IMF-fixdate 포맷 |
| `http_date_parse` | `src/util.c:149` | IMF-fixdate 파싱 |
| `find_crlf` | `src/http_request.c:12` | CRLF 탐색 |
| `trim_ows` | `src/http_request.c:21` | 앞뒤 공백 제거 |
| `ci_equal` | `src/http_request.c:30` | 대소문자 무시 비교 |
| `ci_contains` | `src/http_request.c:35` | 대소문자 무시 부분 검색 |
| `parse_method` | `src/http_request.c:46` | 메서드 판별 |
| `parse_version` | `src/http_request.c:58` | 버전 검증 |
| `parse_request_line` | `src/http_request.c:74` | 요청 라인 분해 |
| `hexval` | `src/http_request.c:113` | 16진수 변환 |
| `decode_uri` | `src/http_request.c:123` | 퍼센트 디코딩 |
| `normalize_path` | `src/http_request.c:152` | `.`/`..` 정규화 |
| `parse_uri` | `src/http_request.c:205` | path/query 분리 |
| `parse_content_length` | `src/http_request.c:239` | CL 검증 |
| `parse_headers` | `src/http_request.c:266` | 헤더 루프 |
| `http_request_init` | `src/http_request.c:342` | 요청 초기화 |
| `http_request_free` | `src/http_request.c:347` | 본문 해제 |
| `http_request_reset` | `src/http_request.c:353` | 재사용 초기화 (미사용) |
| `http_request_parse_buffer` | `src/http_request.c:358` | 헤더 파싱 진입점 |
| `http_request_read_body` | `src/http_request.c:472` | 본문 읽기 |
| `http_request_get_header` | `src/http_request.c:543` | 헤더 조회 |
| `http_status_reason` | `src/http_response.c:7` | 상태 → reason |
| `http_response_init` | `src/http_response.c:31` | 응답 초기화 |
| `http_response_set_status` | `src/http_response.c:35` | 상태 설정 (미사용) |
| `http_response_add_header` | `src/http_response.c:40` | 헤더 누적 |
| `http_response_send_headers` | `src/http_response.c:58` | 상태 라인 + 헤더 전송 |
| `http_response_send_redirect` | `src/http_response.c:80` | 301 |
| `http_response_send_options` | `src/http_response.c:101` | 200 + Allow |
| `http_response_send_memory` | `src/http_response.c:119` | 메모리 본문 응답 |
| `http_response_send_error` | `src/http_response.c:220` | HTML 에러 |
| `send_file` | `src/static_file.c:17` | 64KB 스트리밍 |
| `has_dotfile_segment` | `src/static_file.c:37` | dotfile 검사 |
| `path_within_root` | `src/static_file.c:55` | root 포함 검사 |
| `status_from_errno` | `src/static_file.c:64` | errno → 상태 |
| `send_error_for_errno` | `src/static_file.c:72` | errno 기반 에러 응답 |
| `parse_size_value` | `src/static_file.c:81` | Range 숫자 파싱 |
| `parse_range_header` | `src/static_file.c:99` | Range 해석 |
| `send_range_error` | `src/static_file.c:151` | 416 |
| `static_file_handle` | `src/static_file.c:194` | 정적 파일 서빙 |
| `path_within_root` (cgi) | `src/cgi.c:22` | cgi_dir 포함 검사 |
| `send_cgi_response` | `src/cgi.c:39` | CGI 출력 → HTTP |
| `build_cgi_env` | `src/cgi.c:189` | CGI 환경변수 |
| `cgi_handle` | `src/cgi.c:252` | CGI 실행 |

---

흐름 설명은 [function_flow.md](function_flow.md)를, 모듈 구조는 [architecture.md](architecture.md)를 본다.
