## Phase 1: TCP 소켓 서버 ~ 동적 요청 처리

`socket → bind → listen → accept` 로 연결을 받고, 요청을 파싱해서, 정적 파일이나 CGI 프로그램의 결과를 응답으로 돌려주는
**순차 처리(연결 하나씩) HTTP 서버**다. 외부 HTTP 라이브러리 없이 표준 C 라이브러리와 POSIX API 만 쓴다.
이 문서는 phase 1 이 끝난 시점의 동작을 기준으로 쓴다. keep-alive 와 동시성은 [phase2.md](phase2.md), 프록시는 [phase3.md](phase3.md) 에 있다.

```
accept
 → read_request_head      CRLFCRLF 까지 반복 read (16KB 버퍼, 전체 5초)
 → http_parse_request     414 → 400 → 501 → 505 → 헤더 검사
 → url_check_path         디코딩(400) → ".."(400) → 점 파일(404)
 → /cgi-bin 인가?
      예     : cgi_resolve → (POST) 411 → 413 → 100 Continue → 본문 읽기 → cgi_run → 응답
      아니오 : POST 면 405 / GET 이면 static_resolve → static_send
 → 응답 후 close (에러 응답이면 lingering close)
```

- 전체를 먼저 설계하지 않고 **모듈 하나씩 "결정 → 구현 → 테스트 → 커밋"** 순서로 만들었다.
  소켓 뼈대 → 파서 → 응답 생성기 → 정적 서빙 → POST 본문 → CGI → 정상 종료 → 기능 테스트 → valgrind.
- 유저 시나리오(S1~S11)를 먼저 쓰고, 각 시나리오가 그대로 기능 테스트가 된다.
- 동작이 달라지는 중요한 설계(상태 코드, 한도, 모듈 경계)는 직접 결정했고, 표준이 정해 둔 값이나 내부 이름 같은 사소한 선택은 구현하면서 정한 뒤 이유와 함께 기록했다.
- **하지 않은 것:** keep-alive, 동시 처리(phase 2), 프록시(phase 3), chunked 전송, HEAD 메서드.

### 과제 요구사항과 이번 선택

| 과제 요구 | 선택 | 구현·검증 |
| --- | --- | --- |
| TCP 소켓 서버: `socket → bind → listen → accept` 직접 구현 | IPv4 `INADDR_ANY`, `SO_REUSEADDR`, `SOMAXCONN`. 연결은 하나씩 순차로 처리 | `server.c`, S1 |
| 요청 라인 파싱 (메서드, URI, 버전) | 완성된 헤더 버퍼를 한 번에 **제자리 파싱**. 오류는 곧바로 상태 코드(400/414/501/505)로 | `http_parser.c`, 단위 113개 |
| 헤더 파싱 (`Host`, `Content-Length`, `Connection`) | 엄격 모드: CRLF 만, 헤더 64개까지, Content-Length 숫자 검증, CL+TE 동시 거부. Connection 은 파싱만 | `http_parser.c`, S7, S8 |
| 본문 처리 (POST) | `Content-Length` 만큼 읽어서 CGI 의 stdin 으로 전달. 상한 1MB, 시간 제한 10초 | `body.c`, S6, S9 |
| 최소 지원 메서드 GET, POST | 그 밖의 메서드는 501 | S7 |
| 상태 라인, 표준 헤더, 본문 송신 | `Date`, `Server`, `Content-Type`, `Content-Length`, `Connection: close`. 헤더만 만들고 본문은 따로 전송 | `http_response.c` |
| 정적 파일 서빙, 루트 디렉터리 지정 | `-r` 로 지정한 루트를 `realpath` 로 정규화한 뒤 그 안에서만 서빙 | `static.c`, S1~S4 |
| MIME 타입 매핑 | 확장자 고정 표 14개, 모르면 `application/octet-stream` | `mime_lookup`, 단위 테스트 |
| 404 / 403 / 500 등 기본 오류 응답 | errno 별 매핑, 고정 템플릿 에러 페이지 | S2, S4, S11 |
| 동적 요청 처리 (CGI 또는 백엔드 전달) | **CGI**: `fork` + `execve`, 환경 변수 새로 구성, 출력은 전부 모은 뒤 서버가 헤더를 작성 | `cgi.c`, `cgi-src/adder.c`, S5, S6, S11 |
| 빌드, 포트·루트를 CLI 로 받기 | `Makefile`, `getopt` 로 `-p`, `-r` | `config.c` |
| 단위·기능·메모리 검사, 엣지 케이스 | 단위 8개 파일 406개 검사, 기능 55개(느린 것까지 65개), ASan·UBSan·LSan, valgrind | `make test` / `ftest` / `asan` / `valgrind` |

### 사용법

```sh
make                            # ./server 와 www/cgi-bin/adder 를 빌드
./server -p 8080 -r ./www       # 둘 다 생략하면 8080, ./www
```

- `-p` 포트 1~65535, `-r` 정적 파일 루트(디렉터리여야 하고 `/` 는 거부). 틀리면 이유와 사용법을 출력하고 exit 1.
- 종료는 `Ctrl+C` 또는 `kill -TERM <pid>`. 처리 중인 요청을 끝낸 뒤 exit 0.

```sh
curl -i localhost:8080/index.html                      # S1
curl -i localhost:8080/nope.html                       # S2: 404
curl -i "localhost:8080/cgi-bin/adder?a=1&b=2"         # S5: 3
curl -i -d 'a=1&b=2' localhost:8080/cgi-bin/adder      # S6: 3
curl -i -X DELETE localhost:8080/index.html            # S7: 501
```

```
HTTP/1.1 200 OK
Date: Mon, 23 Nov 2026 05:07:47 GMT
Server: week13-webserver
Content-Type: text/plain; charset=utf-8
Content-Length: 2
Connection: close

3
```

접근 로그는 stderr 에 요청마다 한 줄이다. `ip:port "요청 첫 줄" 상태 본문바이트 [비고]`.

```
127.0.0.1:39726 "GET /cgi-bin/adder?a=1&b=2 HTTP/1.1" 200 2
127.0.0.1:39806 "GET /cgi-bin/crash HTTP/1.1" 500 180 (cgi signal 11)
127.0.0.1:39808 "GET /cgi-bin/slow HTTP/1.1" 500 180 (cgi timeout)
127.0.0.1:46102 "POST /cgi-bin/adder HTTP/1.1" - 0 (client closed mid-body)    # 응답 없이 닫음: 상태 자리에 -
```

| Make 타깃 | 하는 일 |
| --- | --- |
| `make test` | 단위 테스트 전체 (자체 assert 매크로) |
| `make asan` | ASan + UBSan + LSan 빌드(`build-asan/`, `server-asan`)로 단위 테스트 |
| `make ftest` | ASan 서버를 임시 문서 루트로 띄워 기능 시나리오 55개 (몇 초) |
| `make ftest-slow` | 시간 제한 시나리오까지 65개 (약 33초) |
| `make valgrind` | 일반 빌드 서버를 valgrind 아래에서 띄워 기능 테스트 |

### 유저 시나리오

| # | 시나리오 | 기대 결과 |
| --- | --- | --- |
| S1 | `GET /index.html` | 200, `text/html`, Content-Length 는 파일 크기 |
| S2 | `GET /nope.html` | 404, 서버가 만든 HTML 에러 페이지 |
| S3 | `GET /cat.png` | 200, `image/png`, 받은 파일이 원본과 바이트 단위로 같음 |
| S4-1 | index.html 없는 디렉터리 | 목록은 보여주지 않음 → 403 |
| S4-2 | `.env` 같은 점 파일 | 접근 금지 → 404 |
| S5 | `GET /cgi-bin/adder?a=1&b=2` | 200, 결과 3 |
| S6 | `POST /cgi-bin/adder`, 본문 `a=1&b=2`, Content-Length 7 | 200, 결과 3 |
| S7 | `DELETE /index.html` | 501 |
| S8 | URI 가 8192바이트를 넘음 (예시 9000자) | 414 |
| S9 | 본문 100바이트 약속하고 10바이트만 보낸 뒤 끊김 | 응답 없음, adder 실행 안 함, 버퍼 free, 서버는 다음 요청 정상 처리 |
| S10 | 100MB 파일 응답 도중 클라이언트가 끊음 | `write` 가 EPIPE, 파일·소켓 close, 서버 생존 |
| S11 | CGI 가 출력 도중 비정상 종료 | 500, CGI 의 부분 출력은 버림 |

### 설계 결정과 이유

| | 선택 | 이유 | 검토한 대안 |
| --- | --- | --- | --- |
| **처리 모델** | 연결 하나씩 순차, 응답은 항상 `Connection: close` | 순차 처리에서 keep-alive 를 켜면 한 클라이언트가 서버를 독점한다. keep-alive 는 동시성과 함께 phase 2 | 처음부터 keep-alive |
| **첫 모듈** | 소켓 뼈대(고정 응답으로 연결부터 curl 확인) | 서버가 실제로 뜨는 걸 먼저 보고 파서·정적 서빙을 얹는다 | 파서 먼저(소켓 없이 테스트 쉬움), MIME 먼저(가장 작지만 동작이 안 보임) |
| **검사 순서** | 414 → 400 → 501 → 505 → 헤더 → 경로 → 파일(404/403), 첫 오류에서 중단 | URI 길이는 요청 줄을 읽는 순간 걸러야 버퍼를 넘기 전에 막는다. Host 없는 `DELETE` 는 400 이 아니라 501 이 된다 | 형식 오류(400) 우선: 헤더까지 다 본 뒤 메서드 판단 |
| **에러 페이지** | 고정 템플릿, 상태 코드와 문구만 | 요청 내용을 넣지 않으니 HTML 이스케이프가 필요 없고 반사형 주입이 원천 차단된다 | 요청 경로 표시, `errors/404.html` 파일(실패 시 내장 템플릿도 필요) |
| **빌드·구조** | `Makefile` + `src/` `include/` `tests/` `www/` | 단순하고 의존성이 없다 | CMake(CTest, IDE 연동이 되지만 설정이 무겁다) |
| **테스트 도구** | 자체 assert 매크로(`tests/test.h`) | 의존성 0. 실패하면 조건식과 줄 번호만 출력 | 단일 헤더 프레임워크(acutest 등): 외부 코드가 레포에 들어온다 |
| **개발 환경** | WSL2 Ubuntu | Docker 는 기본 사용자가 root 라 `chmod 000` 을 무시하고 읽어서 권한 403 을 재현할 수 없다 | Docker |

#### 소켓과 요청 읽기 (`server.c`, `config.c`)

| | 선택 | 이유 | 검토한 대안 |
| --- | --- | --- | --- |
| **bind 주소** | IPv4 `INADDR_ANY` | 과제 범위에 충분하고 단순하다. WSL 에서 Windows 브라우저로도 접근된다 | `getaddrinfo`(IPv6 대비지만 코드가 길어짐), `127.0.0.1` 만(외부 접근 불가) |
| **SO_REUSEADDR** | 항상 설정 | 재시작 직후 `TIME_WAIT` 이 남아 있어도 같은 포트에 bind 된다 | 없이 bind 실패를 재현해 보기 |
| **실행 인자** | `getopt` 로 `-p`, `-r`. 기본 8080, `./www`. 틀리면 이유 + 사용법 후 exit 1 | 옵션은 늘려도 순서에 의존하지 않는다 | 위치 인자, 처음부터 설정 파일(파서가 추가 작업) |
| **루트** | `realpath` 로 절대 경로 정규화 + 디렉터리 확인. `-r /` 는 거부 | 이후 모든 "root 안인가" 검사의 기준. `-r /..` 처럼 결과가 `/` 인 경우도 거부해 파일 시스템 전체 노출을 막는다 | 특수 처리로 허용 |
| **SIGPIPE** | 무시 | 끊긴 소켓에 write 해도 프로세스가 죽지 않고 `EPIPE` 를 받는다 (S10 의 전제) | 기본 동작 |
| **요청 읽기** | CRLFCRLF 가 나올 때까지 반복 `read`, 16KB 고정 버퍼 | TCP 는 한 번의 read 로 요청이 다 온다는 보장이 없다. 읽지 않은 데이터가 남은 채 close 하면 RST 가 나간다 | read 한 번, 안 읽고 응답 |
| **경계 검색** | 새로 받은 부분만 보되 3바이트 겹침 | `\r\n\r\n` 이 두 read 에 걸쳐 와도 놓치지 않는다 | 매번 처음부터 |
| **버퍼 크기** | 16KB | URI 상한(8KB)보다 넉넉해 헤더 자리가 남는다. 가득 차도 헤더 끝이 없으면 요청 줄이 끝났는지로 414/431 구분 | 8KB(414/431 경계가 모호), 32KB 이상(동시성에서 연결마다 메모리) |
| **헤더 시간 제한** | `poll` + `CLOCK_MONOTONIC`, 헤더 **전체** 5초 | read 한 번이 아니라 전체 기준이라 1바이트씩 천천히 보내는 공격(slowloris)도 끊긴다. 받은 게 0바이트면 응답 없이 close, 1바이트 이상이면 408 | `SO_RCVTIMEO`(read 한 번 기준이라 못 막음), 항상 응답 없이 close |
| **에러 응답 뒤 닫기** | **lingering close**: `shutdown(SHUT_WR)` 뒤 남은 수신 데이터를 최대 64KB / 1초 비우고 close | 읽지 않은 데이터가 남은 채 close 하면 RST 로 응답이 유실된다. 20000자 URI 에 414 가 0바이트로 사라지던 문제를 이걸로 해결했다 | 바로 close, 모든 응답에 lingering(성공 응답은 불필요) |
| **정상 종료** | `SIGINT`/`SIGTERM` 핸들러는 플래그만 켠다(`SA_RESTART` 없음). 막힌 `accept` 가 `EINTR` 로 돌아오면 루프를 빠져나와 exit 0. 처리 중인 요청은 끝까지 | 종료 경로로 정리돼야 LSan·valgrind 가 누수를 보고한다. 모든 대기 지점에 플래그 검사를 넣으면 하나만 빠져도 의미가 없다 | 즉시 중단, 아예 안 넣기(누수 보고가 안 나옴) |
| **fd 상속** | listen·연결 소켓에 `FD_CLOEXEC` | CGI 자식이 소켓을 물려받지 않는다 | 자식에서 일괄 close |

#### 요청 파싱 (`http_parser.c`)

| | 선택 | 이유 | 검토한 대안 |
| --- | --- | --- | --- |
| **파서 입력** | 완성된 헤더 버퍼를 한 번에 | 스트리밍 파서보다 단순하고 테스트하기 쉽다 | 상태 머신 점진 파서(긴 줄을 도착 즉시 거르지만 복잡) |
| **메모리** | 제자리 파싱: 구분자 자리에 NUL 을 쓰고 버퍼 안 포인터만 저장 | 복사도 `malloc` 도 없어서 `free` 누락이 없다. 대신 구조체는 버퍼보다 오래 쓰면 안 된다 | 고정 배열에 복사(구조체 16KB 이상), `malloc` 문자열 |
| **헤더 저장** | 고정 배열 64개 + 대소문자 무시 조회 함수. 65번째는 431 | CGI 와 프록시에서 전체 헤더가 필요해져도 다시 짜지 않는다 | 필요한 헤더만 추출, 동적 리스트 |
| **오류 표현** | 상태 코드를 그대로 반환 (0 이면 성공) | 변환 계층이 없어 호출부가 단순하다 | 오류 enum + 변환 함수 |
| **버전** | 1.0 과 1.1 허용, 다른 버전은 505, 형식이 틀리면 400 | `ab` 가 기본으로 HTTP/1.0 을 보낸다. 1.1 만 받으면 부하 테스트를 못 한다 | 1.1 만, 1.0·1.1 외 모두 400 |
| **메서드** | GET, POST 만 (대소문자 구분). 나머지는 501 | 과제의 최소 범위 | HEAD 지원(정적·CGI 모두 분기가 늘어남) |
| **줄 끝** | CRLF 만. LF 단독은 400 | 프록시와 서버가 줄 끝을 다르게 해석하는 요청 스머글링 여지를 없앤다. 대가: `nc` 로 손으로 칠 때는 `nc -C` | LF 단독 허용(RFC 의 MAY) |
| **헤더 검증** | 엄격: obs-fold 거부, 이름은 token 만(콜론 앞 공백도 400), HTAB 외 제어 문자 400, obs-text 허용 | 모호한 입력을 받아 주지 않는 쪽이 안전하다 | 필수만 검사 |
| **Content-Length** | 숫자만(`+5`, `-1`, 빈 값, 오버플로는 400, `strtoll` + `ERANGE`). 같은 값 중복은 허용, 다르면 400 | 길이가 모호하면 다음 요청의 경계가 흔들린다 | 느슨하게 `atoi` |
| **Transfer-Encoding** | Content-Length 와 같이 오면 400, 단독이면 501 | CL+TE 는 스머글링 전형. chunked 는 이번 범위 밖 | chunked 지원 |
| **Host** | HTTP/1.1 에서 없거나 두 개 이상이면 400 (1.0 은 없어도 됨) | 값이 서버 이름과 맞는지는 phase 3 라우팅에서 | 존재만 허용 |
| **URI** | origin-form(`/` 로 시작)만. 첫 `?` 로 path 와 query 를 나누고 디코딩은 나중에. 8192 초과는 414 | 주요 서버 기본값과 비슷하고 정상 URL 은 거의 다 받는다. absolute-form 과 `*` 는 400(forward proxy 에서 확장할 지점) | 파서에서 퍼센트 디코딩(원문을 따로 보관해야 함) |

#### 응답 생성 (`http_response.c`)

| | 선택 | 이유 | 검토한 대안 |
| --- | --- | --- | --- |
| **인터페이스** | **헤더만** 만들고 본문은 따로 전송 | 100MB 파일도 메모리에 올리지 않고 스트리밍한다 | 전체 응답을 한 버퍼에 조립 |
| **헤더 구성** | 상태 줄, `Date`(IMF-fixdate), `Server`, `Content-Type`, `Content-Length`, [추가 헤더], `Connection: close` | Date 는 시계가 있는 서버의 MUST. 시각을 인자로 받아 테스트에서 헤더 전체를 바이트 단위로 비교한다 | 필수 3개만 |
| **추가 헤더** | 문자열 인자로 `Location`(301), `Allow`(405) | 헤더 몇 개 때문에 구조체를 만들 필요가 없다 | 응답 구조체 + send 함수 |
| **알 수 없는 상태 코드** | 500 으로 바꾸고 stderr 에 `BUG: unknown status` | 서버 버그를 조용히 넘기지 않는다 | `assert` 로 중단 |
| **헤더 버퍼 부족** | `BUG: response head overflow` 를 찍고 응답 없이 close | 잘린 응답을 보내느니 닫는다 | 잘라서 전송 |
| **접근 로그** | stderr 한 줄. 요청 첫 줄은 파싱 **전에** 원문에서 최대 200바이트 복사, 출력 불가 문자와 `"` 는 `?` 로 | 파싱에 실패한 요청도 무엇이었는지 남고, 로그 주입·형식 깨짐을 막는다 | 파싱 실패는 `-` 만 |

#### 정적 파일 (`url.c`, `static.c`)

| | 선택 | 이유 | 검토한 대안 |
| --- | --- | --- | --- |
| **경로 해석** | 퍼센트 디코딩(잘못된 `%`·`%00` 은 400) → `..` 세그먼트 400 → `.` 으로 시작하는 세그먼트 404 → `realpath` 로 루트 밖이면 403 | **2중 방어**: 문자열 검사만으로는 루트 안 심볼릭 링크의 탈출을 못 막고, `realpath` 만으로는 `..` 가 파일 시스템까지 내려간다 | 디코딩 없이 원문, 세그먼트 검사만 |
| **디코딩** | 한 번만, `%2F` 는 `/` 로 디코딩한 뒤 검사. `+` 는 경로에서 일반 문자 | `%2e%2e`, `..%2F` 도 걸리고, 이중 디코딩 우회(`%252e`)가 없다 | `+` 를 공백으로 |
| **S4-1 디렉터리** | index.html 이 없으면 **403** | 목록을 보여주지 않는 정책. "있지만 보여줄 수 없음"이 의미상 정확하고 nginx(autoindex off)와 같다 | 404 |
| **S4-2 점 파일** | **404** | 파일 시스템을 보기 전에 경로 문자열로 판단하므로 `.env` 가 있든 없든 응답이 같다 → 존재 여부가 새지 않는다 | 403 |
| **끝 `/` 없는 디렉터리** | **301** `Location: 경로/`(쿼리 유지) | index.html 안의 상대 경로(`style.css`)가 올바르게 풀린다 | 그대로 제공(상대 경로 깨짐), 404 |
| **정적 경로의 POST** | **405** + `Allow: GET` | 방법은 알지만 이 자원에는 허용되지 않는다 | 파일 제공, 501 |
| **오류 매핑** | ENOENT·ENOTDIR 404, EACCES 403, 일반 파일·디렉터리가 아니면 403, 그 외 errno 와 PATH_MAX 초과는 500 | `stat` 으로 종류를 먼저 확인한 뒤 `open` 해서 FIFO 에서 멈추지 않는다. Content-Length 는 `open` 후 `fstat` 기준 | 모두 404 |
| **큰 파일 전송 (S10)** | 64KB 고정 버퍼로 read → `write_all`. 실패하면 파일·소켓을 닫고 로그 `(aborted)` | 메모리가 일정하다. 이미 헤더를 보냈으니 상태 코드는 못 바꾼다. 전송 중 파일이 줄면 약속한 길이를 못 지키므로 그냥 close | `sendfile`(리눅스 전용), `mmap`(전송 중 파일이 잘리면 SIGBUS) |
| **MIME** | 확장자 고정 표, 대소문자 무시. `.html` 처럼 이름이 점으로 시작하고 다른 점이 없으면 확장자 없음 | 외부 파일 없이 동작. `text/*` 는 `charset=utf-8` | `mime.types` 파일 읽기 |

| 확장자 | Content-Type |
| --- | --- |
| html, htm | `text/html; charset=utf-8` |
| css | `text/css; charset=utf-8` |
| js | `text/javascript; charset=utf-8` (RFC 9239 권장값) |
| json | `application/json; charset=utf-8` |
| txt | `text/plain; charset=utf-8` |
| png / jpg·jpeg / gif / svg / ico / webp | `image/png` / `image/jpeg` / `image/gif` / `image/svg+xml` / `image/x-icon` / `image/webp` |
| pdf | `application/pdf` |
| 그 외, 확장자 없음 | `application/octet-stream` |

#### POST 본문 (`body.c`)

| | 선택 | 이유 | 검토한 대안 |
| --- | --- | --- | --- |
| **범위** | 본문을 실제로 읽는 곳은 `/cgi-bin` POST 뿐 | 정적 경로 POST 는 405 로 끝나고 남은 본문은 lingering close 가 비운다 | 모든 POST 의 본문을 읽기 |
| **검사 순서** | 경로 → 스크립트 찾기 → Content-Length 없음 **411** → 1MB 초과 **413** → 100 Continue → 본문 읽기 | 413 은 Content-Length 값만 보고 본문을 읽기 전에 보낸다. 없는 스크립트에는 본문을 받지 않고 404 | 길이 없으면 0 으로 취급 |
| **상한** | 1MB | 폼 입력엔 충분하고 `malloc` 한 번에 부담이 없다 | 64KB(작은 업로드도 막힘), 8MB(동시성에서 연결 수만큼 곱해짐) |
| **시간 제한** | 헤더를 다 받은 **뒤** 새로 시작하는 본문 전용 10초. 넘으면 408 | 헤더는 온전히 받았으니 응답할 수 있다 | 헤더 5초를 이어서, 응답 없이 close |
| **S9 끊김** | 응답 없이 close, 버퍼는 즉시 free, CGI 는 실행하지 않음. 로그 `(client closed mid-body)` | 받을 사람이 없고 데이터가 불완전하다 | — |
| **Expect: 100-continue** | 411·413 을 통과했고, HTTP/1.1 이고, 본문이 아직 덜 왔을 때만 `100 Continue` 를 먼저 보낸다. 다른 Expect 값은 무시 | curl 은 큰 본문에 이 헤더를 붙이고 최대 1초를 기다린다. 처리하지 않으면 요청마다 1초 느려진다. 413 이면 본문을 보내기 전에 거절을 받는다. 1.0 에는 보내지 않는다(RFC 9110) | 무시, 모르는 값은 417 |
| **버퍼** | `Content-Length` 만큼 `malloc`, 길이 0 이면 할당 안 함, 모든 경로에서 `free`. 실패하면 500 | 연결별로 자연스럽게 분리되어 동시성에서도 그대로 쓸 수 있다 | 1MB 정적 버퍼(공유 불가) |
| **함수 모양** | `read_request_body`(저장)와 `discard_request_body`(읽고 버림)를 따로 두고 내부 루프를 공유. 입력은 `struct body_input`, `*consumed`(헤더 버퍼에서 쓴 바이트 수)를 돌려줌 | 헤더 버퍼에 같이 들어온 본문 앞부분을 먼저 쓰고 Content-Length 보다 많이 읽지 않는다. `consumed` 와 `discard` 는 phase 2 의 pipelining·keep-alive 에서 쓰려고 미리 만들었다 | 하나의 함수 + 모드 인자 |

#### CGI (`cgi.c`, `cgi-src/adder.c`)

| | 선택 | 이유 | 검토한 대안 |
| --- | --- | --- | --- |
| **찾기** | `/cgi-bin/<이름>` 한 단계만. `/cgi-bin/adder/extra` 는 404. `realpath` 가 `root/cgi-bin` 밖이면 403, 일반 파일이 아니거나 실행 권한이 없으면 403 | PATH_INFO 탐색 로직이 필요 없고 공격 표면이 작다 | PATH_INFO 지원 |
| **실행** | `fork` + `execve`, **셸을 거치지 않음**. stdin·stdout 은 파이프, stderr 는 서버 stderr 를 물려받음 | 셸 주입 여지가 없고 CGI 의 오류가 서버 로그에 남는다 | `system`, `popen` |
| **환경 변수** | 서버 환경은 물려주지 않고 새로 구성: `GATEWAY_INTERFACE`, `SERVER_*`, `REQUEST_METHOD`, `QUERY_STRING`, `SCRIPT_NAME`, `REMOTE_*`, `PATH`, POST 면 `CONTENT_LENGTH`·`CONTENT_TYPE` | 서버의 비밀이 새지 않는다. 요청 헤더는 `HTTP_*` 로 넘기지 않는다(`HTTP_PROXY` 를 가로채는 httpoxy 도 회피) | 환경 상속, `HTTP_*` 전달 |
| **자식 준비** | SIGPIPE 를 기본값으로 복원, 작업 디렉터리를 `cgi-bin` 으로, exec 실패는 `_exit(127)` | 무시 설정은 exec 뒤에도 상속되므로 CGI 가 EPIPE 를 받지 못한다 | — |
| **입출력** | stdin 쓰기와 stdout 읽기를 `poll` 하나로 **동시에**. stdin 은 non-blocking. CGI 가 stdin 을 다 안 읽어서 생기는 EPIPE 는 오류가 아님 | 1MB 본문은 파이프(64KB)보다 크다. CGI 가 출력을 먼저 쓰는데 서버가 본문부터 쓰면 서로 기다리며 멈춘다(교착) | 쓰기 → 읽기 순서로 |
| **출력** | **전부 버퍼링**한 뒤 서버가 상태 줄과 헤더를 작성 | CGI 가 중간에 죽으면 이미 보낸 것 없이 500 으로 바꿀 수 있다(S11) | 스트리밍 |
| **한도** | 출력 1MB, 실행 5초(fork 부터). 넘으면 `SIGKILL` 후 500. 성공이든 실패든 항상 `waitpid` | 좀비를 남기지 않고, 느린 CGI 가 순차 서버를 오래 막지 못한다 | 8MB / 30초, 시간 초과만 504 |
| **실패 기준 (S11)** | 시그널로 죽었거나 exit 코드가 0 이 아니면 출력을 버리고 500 | 출력이 멀쩡해 보여도 종료 코드가 실패면 믿지 않는다 | 시그널 종료만 |
| **CGI 헤더** | `Content-Type` 필수(한 번, 256바이트 이하), `Status` 선택. 그 밖의 헤더는 버림. 줄 끝은 LF·CRLF 모두 허용 | 서버가 쓰는 헤더(Date, Content-Length, Connection)와 겹치지 않는다. 값에 CR 등 제어 문자가 있으면 500 이라 응답 헤더 주입을 막는다 | 나머지 헤더 전달 |
| **Status** | 200~599 의 세 자리 숫자만. **1xx·204·205·304 는 500.** 문구는 서버가 정한다(표에 없으면 빈 문구 `HTTP/1.1 201 `) | 1xx 는 최종 응답이 될 수 없고, 204/205/304 는 본문을 가질 수 없다. CGI 가 보낸 문구는 믿지 않는다 | 1xx 통과, 본문을 버리고 204 허용 |
| **adder** | C 로 작성, make 가 `www/cgi-bin/adder` 로 빌드. GET 은 `QUERY_STRING`, POST 는 stdin. 성공하면 `text/plain` 으로 `3`, a·b 가 없거나 중복이거나 숫자가 아니거나 범위를 넘으면 `Status: 400`(exit 0) | 외부 인터프리터가 필요 없다. 400 도 정상 응답이므로 exit 0 이어야 한다(exit != 0 이면 서버가 500 으로 바꾼다) | 셸 스크립트, Python |

### 과제 중점 포인트와 이 구현

**소켓 프로그래밍의 기본 흐름**

- `socket → setsockopt(SO_REUSEADDR) → bind → listen → accept`. 단계마다 실패하면 단계 이름으로 `perror` 한다.
  이미 쓰는 포트에 `bind` 하면 `EADDRINUSE` 로 시작에 실패하고(exit 1), `SO_REUSEADDR` 덕분에 종료 직후 재시작은 `TIME_WAIT` 이 남아 있어도 성공한다.
- 3-way handshake 는 커널이 `accept` 전에 끝낸다. `accept` 는 완성된 연결을 큐에서 꺼낼 뿐이고, 큐가 비어 있으면 블로킹된다.
  종료 신호를 `SA_RESTART` 없이 받는 이유가 이것이다. 막힌 `accept` 를 `EINTR` 로 깨워야 루프를 빠져나올 수 있다.
- **블로킹과 논블로킹:** 클라이언트 소켓은 블로킹으로 쓰되 읽기 전에 `poll` 로 **남은 시간만큼만** 기다려서 시간 제한을 건다.
  `read` 에 `SO_RCVTIMEO` 를 걸면 한 번의 `read` 기준이라 조금씩 보내는 클라이언트를 못 막는다.
  논블로킹은 CGI 의 stdin 파이프에서만 쓴다. 파이프가 차서 `write` 가 막히면 stdout 읽기를 못 하는 교착을 피하기 위해서다.
- 데드라인은 처음에 한 번만 정하고 `poll` 마다 남은 시간을 다시 계산한다(`EINTR` 로 깨어나도 재계산). `CLOCK_MONOTONIC` 이라 시스템 시간을 바꿔도 흔들리지 않는다.

**HTTP 명세: 경계를 어떻게 식별하나**

- **요청 라인·헤더의 끝**은 `\r\n\r\n` 이다. TCP 는 바이트 스트림이라 이 네 바이트가 두 번의 `read` 에 걸쳐 올 수 있어서 3바이트를 겹쳐 검색한다.
  끝을 찾지 못한 채 버퍼(16KB)가 차면 요청 라인이 끝났는지로 414(URI 가 김)와 431(헤더가 큼)을 나눈다.
- **본문의 끝**은 `Content-Length` 다. 숫자 검증과 중복·CL+TE 검사를 엄격하게 해서 경계가 모호한 요청을 받지 않는다.
  헤더 버퍼에 같이 들어온 본문 앞부분을 먼저 쓰고, 모자란 만큼만 소켓에서 읽고, Content-Length 보다 많이 읽지 않는다.
- **응답의 끝**도 `Content-Length` 다. 정적 파일은 파일 크기, CGI 는 출력을 다 모은 뒤 길이를 붙인다.
- **`Connection: keep-alive`:** "이 TCP 연결을 계속 쓰자"는 뜻이다. phase 1 은 헤더를 파싱해서 값만 보관하고 응답은 항상 `Connection: close` 다.
  순차 처리 서버에서 연결을 붙들고 있으면 다른 클라이언트가 서버 전체를 기다리게 되기 때문이다. 응답 경계(Content-Length)와 `consumed`, `discard_request_body` 는 phase 2 에서 keep-alive 에 쓰려고 미리 맞춰 두었다.
- **RFC 7230~7235 의 최소 범위:** GET·POST, HTTP/1.0·1.1, Content-Length 본문, `Host`, `Connection`, `Expect: 100-continue`. chunked 와 HEAD, 조건부 요청, 범위 요청은 범위 밖이다.

**동시성 모델, 프록시**

- phase 1 은 `accept → 처리 → close` 를 연결 하나씩 순차로 돈다. 그래서 느린 클라이언트나 큰 파일 하나가 서버 전체를 막는다.
  이 한계를 시간 제한(헤더 5초, 본문 10초, CGI 5초)으로 줄여 두고, 근본 해결(fork / 스레드 풀 / epoll 비교, C10K)은 [phase 2](phase2.md) 에서 다룬다.
- Proxy 와 Reverse Proxy 의 차이는 [phase 3](phase3.md) 에서 다룬다.

**메모리·자원 관리**

- 요청 파싱은 제자리 파싱이라 `malloc` 이 없다. `malloc` 은 POST 본문 버퍼와 CGI 출력 버퍼뿐이고, 모든 경로(성공, 시간 초과, 끊김, CGI 실패)에서 `free` 한다.
- 파일 fd 와 소켓은 전송이 끝나거나 실패하는 모든 경로에서 닫는다. S9, S10 이 그 경로를 지나간 뒤에도 서버가 다음 요청을 처리하는지 확인한다.
- 정상 종료(`SIGINT`/`SIGTERM`)가 있어야 종료 시점에 LeakSanitizer 와 valgrind 가 누수를 보고한다. 종료하다 죽으면 보고가 나오지 않는다.
  LSan 이 이 환경에서 실제로 누수를 잡는지는 일부러 누수를 낸 프로그램으로 확인했고, valgrind 도 종료 직전에 100바이트 누수를 넣어 exit 99 로 실패하는 것을 확인했다.

**엣지 케이스**

| 케이스 | 결과 |
| --- | --- |
| 헤더 일부만 보내고 대기 | 5초 뒤 **408** |
| 아무것도 안 보내고 연결만 유지 | 5초 뒤 응답 없이 close |
| 요청 라인이 8192바이트 초과 / 버퍼(16KB)를 넘는 20000자 | **414** (lingering close 덕분에 응답이 전달됨) |
| 큰 `Cookie` 헤더, 헤더 65개 | **431** |
| `HTTP/2.0`, `HTTP/abc` | **505**, **400** |
| `DELETE`, `Transfer-Encoding: chunked` | **501** |
| HTTP/1.1 에 Host 없음, Content-Length 오류(`+5`, `-1`, 오버플로) | **400** |
| Content-Length 없는 CGI POST / 1MB 초과 / 본문 도중 끊김 / 본문이 느림 | **411** / **413** / 응답 없이 close(S9) / **408** |
| `..`, `%2e%2e`, 잘못된 `%`, `%00` | **400** |
| `.env`, root 밖을 가리키는 심볼릭 링크 | **404**, **403** |
| 경로가 `PATH_MAX` 초과 | **500** |
| 100MB 전송 도중 클라이언트가 끊음 | 로그 `(aborted)`, 서버 생존(S10) |
| CGI 크래시 / exit 1 / exec 실패 / 5초 초과 / 출력 1MB 초과 / 헤더 형식 오류 | 모두 **500**, 부분 출력은 버림 |
| CGI 가 `Status: 204` 또는 `Status: 100` | **500** |

### 테스트

**단위 테스트** (`make test`, `tests/test.h` 의 자체 assert 매크로) — 8개 파일, 406개 검사

| 파일 | 검사 | 주요 내용 |
| --- | --- | --- |
| `test_config.c` | 22 | 기본값, 포트 경계, 잘못된 root, `-r /` 거부 |
| `test_http_parser.c` | 113 | 414 경계(8192/8193), 400 형식들, 501/505, 431(64/65), Content-Length·TE·Host 규칙 |
| `test_http_response.c` | 54 | 고정 시각으로 헤더 전체 비교, 버퍼 경계(필요한 크기 -1 / 딱 맞음), 에러 페이지, raw 헤더 |
| `test_server.c` | 23 | 헤더 끝 검색, read 경계 분할, 끊김, 버퍼 초과, 타임아웃 3종(무응답 / 일부 / 전체 기준) |
| `test_url.c` | 55 | 디코딩, 세그먼트 검사, `url_check_path`, `url_is_cgi` |
| `test_static.c` | 38 | 임시 디렉터리로 200/301/403/404/500, 심볼릭 링크, MIME |
| `test_body.c` | 23 | 버퍼에 다 옴, 분할 도착, 길이 0, 도중 EOF, 시간 초과, 버리기, `consumed` |
| `test_cgi.c` | 78 | 스크립트 찾기, 출력 해석, 실행(교착 방지, 크래시, exit 1, exec 실패, 시간 초과, 출력 초과) |

소켓 없이 파이프(`pipe`, `fork`)로 읽기 함수를 검증하고, 파서·응답 생성기·경로 검사·출력 해석처럼 입출력이 없는 부분은 순수 함수로 나눠서 테스트했다.

**기능 테스트** (`tests/functional.sh`, bash + curl)

- 실행할 때마다 `/tmp` 에 임시 문서 루트를 만든다: index.html, 랜덤 바이트 cat.png, 100MB 파일, 빈 디렉터리, `.env`, 루트 밖 링크, adder 와 테스트용 CGI(크래시, Status 201/204, 실행 권한 없음, sleep, 본문 길이 에코).
- `make ftest`: 빠른 시나리오 55개(정적 20, 파싱 7, POST 본문 7, CGI 14, S10 4, 종료 3). `make ftest-slow`: 헤더 408, 무응답 close, 본문 408, CGI 시간 초과, 처리 중 `SIGINT` 까지 65개(약 33초).
- 검사가 하나라도 실패하거나, SIGTERM 종료 코드가 0 이 아니거나, 서버 로그에 sanitizer 보고가 있으면 exit 1. 실패하면 서버 로그를 출력한다.

**메모리 검사**

- `make asan`: AddressSanitizer + LeakSanitizer + UBSan. 일반 빌드와 오브젝트가 섞여 링크가 실패한 적이 있어서 `build-asan/`, `server-asan` 으로 분리했다.
- `make valgrind`: 일반 빌드 서버를 valgrind(`--leak-check=full`, 확실한 누수만 오류, 오류가 있으면 exit 99) 아래에서 띄워 기능 테스트를 그대로 돌린다. 55개 모두 통과, 보고 없음.

### 구현 중 겪은 문제

- **에러 응답이 0바이트로 사라짐:** 20000자 URI 에 414 를 보냈는데 클라이언트가 응답을 못 받았다. 읽지 않은 요청 데이터가 남은 채 close 해서 RST 가 나간 것이었다. → lingering close.
- **`usleep` 이 선언되지 않음:** `-D_POSIX_C_SOURCE=200809L` 에서는 `usleep` 이 없다. → `nanosleep`.
- **ASan 오브젝트가 일반 빌드에 섞여 링크 실패:** → 빌드 디렉터리와 바이너리 이름 분리.
- **CGI 테스트 프로세스가 SIGPIPE 로 죽음:** stdin 을 안 읽는 CGI 에 1MB 를 쓰다 생겼다. → 서버처럼 테스트도 SIGPIPE 무시.
- **누수 주입 실험이 안 걸림:** `-O2` 가 쓰이지 않는 `malloc` 을 지웠다. → `volatile` 포인터로 실험.
- **백그라운드 서버가 `kill -INT` 로 안 죽음:** 비대화형 셸은 백그라운드 작업의 SIGINT 를 무시한다. → 핸들러를 직접 등록해서 덮어씀.
- **WSL 에서 겪은 것:** `/mnt/d` 의 파일은 모두 실행 가능으로 보이고, WSL 안의 git 은 소유자 경고를 낸다. Git Bash 에서 WSL 로 명령을 넘기면 `$` 가 바깥에서 먼저 풀려서 스크립트 파일로 실행했다.
- **`core.filemode=false`:** 그냥 add 하면 `tests/valgrind-server.sh` 가 실행 권한 없이(100644) 커밋된다. → `git add --chmod=+x` 로 100755 확인.

### 상수

| 이름 | 값 | 위치 |
| --- | --- | --- |
| `HTTP_URI_MAX` | 8192 (넘으면 414) | `http_parser.h` |
| `HTTP_MAX_HEADERS` | 64 (넘으면 431) | `http_parser.h` |
| `REQUEST_HEAD_MAX` | 16KB | `server.h` |
| `HEADER_TIMEOUT_MS` | 5000 | `server.h` |
| `LINGER_MAX_BYTES` / `LINGER_TIMEOUT_MS` | 64KB / 1000 | `server.h` |
| `REQUEST_BODY_MAX` | 1MB (넘으면 413) | `server.h` |
| `BODY_TIMEOUT_MS` | 10000 | `server.h` |
| `REQLINE_LOG_MAX` | 200 | `server.h` |
| `STATIC_CHUNK` | 64KB | `static.h` |
| `CGI_OUTPUT_MAX` / `CGI_TIMEOUT_MS` / `CGI_CONTENT_TYPE_MAX` | 1MB / 5000 / 256 | `cgi.h` |

### 알려진 한계와 phase 2 로 넘긴 것

- **순차 처리:** 한 연결을 처리하는 동안 다른 연결은 기다린다. 최악의 경우 약 21초(헤더 5 + 본문 10 + CGI 5 + 비우기 1)다. → phase 2 의 스레드 풀.
- **keep-alive 없음:** 응답은 항상 `Connection: close`. `discard_request_body` 와 `consumed` 는 만들어 두기만 했고 phase 1 에서는 호출하지 않는다.
- **정상 종료가 최소 구현이다:** 처리 중인 요청은 끝까지 기다리고, 두 번째 신호도 같은 동작이다. 터미널의 `Ctrl+C` 는 프로세스 그룹 전체에 가서 실행 중인 CGI 도 죽는다(그 요청은 500 `(cgi signal 2)`). `kill -TERM <서버 pid>` 는 이 문제가 없다. 플래그를 확인한 직후 `accept` 에 들어가기 전에 신호가 오면 다음 연결까지 기다리는 짧은 틈도 있다. → phase 2 에서 종료를 다시 설계.
- **정적 전송 버퍼(64KB)가 `static` 변수:** 연결을 하나씩 처리해서 지금은 안전하지만 동시성에서는 연결별로 바꿔야 한다.
- **점 파일 검사는 요청 경로 문자열만 본다:** 루트 안에 `.env` 를 가리키는 심볼릭 링크를 만들면 그 이름으로는 보인다. 관리자가 만든 링크는 허용하는 구조다.
- **WSL 의 `/mnt/d`:** 모든 파일이 실행 가능으로 보여 "실행 권한 없음 → 403" 이 동작하지 않는다(리눅스 파일 시스템에서는 정상). 단위 테스트는 `/tmp` 에서 돌려서 확인했다.
- **Host 값 검사와 라우팅, 백엔드 전달:** phase 3.
