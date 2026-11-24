# Phase 1 품질 게이트

> 목적: Phase 2(동시성)로 넘어가기 전에 **단위/통합 테스트, 메모리 안전성, fd 누수, 부하, 이식성**을 확인한다.

## 실행 명령

```bash
make test          # 단위 테스트
make integration   # 실제 서버 + curl/nc 통합 테스트
make shutdown-test # SIGINT/SIGTERM/keep-alive/CGI 종료 테스트
make sanitize      # ASan/UBSan 빌드로 test + integration + shutdown-test
make fdtest        # fd 누수 검사
make load          # ab 부하 테스트
```

> `make sanitize`는 clean 후 sanitizer 빌드로 실행한다. 끝나면 `make clean && make`로 일반 빌드로 돌아온다.

## 현재 결과 (2026-11-20 ~ 11-23, macOS)

| 항목 | 명령 | 결과 |
|---|---|---|
| 단위 테스트 | `make test` | **123 assertions, 0 failed** |
| 통합 테스트 | `make integration` | **전체 PASS** (정적/D12/D10/D11/CGI/keep-alive/큰 파일/끊김/truncate/send timeout/에러 keep-alive) |
| ASan/UBSan | `make sanitize` | **PASS** (test + integration + shutdown-test) |
| fd 누수 | `make fdtest` | **PASS** (`before=4, after=4`) |
| 부하 (close) | `make load` | **PASS** (`failed=0`, 약 2,131 RPS) |
| 부하 (keep-alive) | `make load` | **PASS** (`failed=0`, 약 2,409 RPS) |
| 종료 처리 | `make shutdown-test` | **PASS** (idle SIGINT/SIGTERM, keep-alive SIGINT, 두 번째 SIGINT) |
| macOS 빌드 | `make` | **PASS** |
| Linux 빌드 | `make linux-test` / CI | **TODO** (Docker 데몬 미실행, CI 워크플로 추가) |
| 아키텍처 | `docs/architecture.md` | **PASS** |
| README | — | **보류** (사용자가 나중에 작성) |

## Phase 1.5 전송 안정성 (P0)

전송 중 실패를 "성공"으로 처리하던 경로를 정리했다.

- **`send_file` 조기 EOF → `-1`.** `fstat` 이후 파일이 줄면 `Content-Length`와 실제 바디가 어긋난다. 헤더를 이미 보냈으므로 상태 코드를 바꿀 수 없고, 연결을 닫는다.
- **핸들러 반환값 전파 (`handler_result_t`).** `HANDLER_CLOSE`/`HANDLER_ERROR`면 `handle_connection`이 `keep = 0`으로 연결을 닫는다. 에러 응답(404/403/416 등)이 `Connection: close`를 보내면서 다음 요청을 읽던 불일치도 함께 해결됐다.
- **`SO_SNDTIMEO` per-write, `request_timeout_sec`(기본 20초) 유지.** 응답을 읽지 않는 클라이언트가 `write`에서 무한정 잠기지 않는다.

### Phase 1.5 하드닝 (2026-11-23)

- **요청 라인/헤더/URI strict validation**: method/header token, obs-fold, CTL/NUL, 중복 Host, `Host :`, `Content-Length: +5` 거부.
- **HEAD body 억제**: parse/static/CGI 에러와 CGI 성공 응답 모두 HEAD면 body 생략.
- **절대 request deadline**: 헤더/본문을 `poll` + `CLOCK_MONOTONIC` 기준으로 제한. read마다 timeout을 새로 주지 않는다.
- **검증(최종)**: 단위 123 assertions, integration 92 PASS, `make sanitize` PASS, `make fdtest` PASS.
- **CGI supervision**: SIGKILL/reap, exit status/시그널 판정, `S_ISREG`+`X_OK` 검사. `term`/`partial` 테스트 추가.
- **envp-before-fork**: 부모에서 `envp`를 만들고 자식은 `execve`만. 서버 환경을 물려주지 않고 `PATH`만 설정.
- **access log**: `ip "METHOD path" status bytes` 형식. `bytes`는 **응답 body 바이트**다. 제어문자/quote/backslash는 sanitize.
- **lingering close**: 4xx/5xx 응답 후 `shutdown(SHUT_WR)` + 최대 64KB/1초 drain.

P0 완료. 다음은 P1(CGI 출력 계약, body preflight/100-continue, 405 Allow/414, docroot/cgi_dir 검증, split 테스트).

### Phase 1.5 P1 (2026-11-24)

- **CGI 출력 계약**: `Status` 3자리/200~599, 중복 `Status`/`Content-Type` 거부, 204/205/304 body 거부, 201 Created reason 추가, 204/304는 `Content-Length`/기본 `Content-Type` 생략, 205는 `Content-Length: 0`만.
- **body preflight**: static/OPTIONS는 body를 읽지 않고, body가 남아 있으면 `Connection: close`; CGI만 body를 읽는다.
- **Expect: 100-continue**: body가 아직 버퍼에 없을 때 `100 Continue` 전송.
- **점진적 body**: `Content-Length` 전체를 한 번에 `malloc`하지 않고 16KB부터 두 배씩 `realloc`.
- **405 Allow**: `Allow: GET, HEAD` 추가.
- **20KB URI → 414**: request line이 끝나지 않은 buffer overflow는 414, header overflow는 431.
- **docroot/cgi_dir**: docroot `/` 거부, docroot와 cgi_dir가 서로 포함 관계면 시작 거부(cgi_dir `/` 포함), docroot 경로의 dot segment는 허용하되 하위 경로 dotfile은 차단.
- **static open**: `stat`→`open`→`fstat` dev/ino 비교 + `O_NOFOLLOW`(가능한 플랫폼).
- **CGI 실행**: `open`+`fstat`으로 regular/execute 확인, Linux는 `fexecve` + `O_CLOEXEC`, macOS는 `execve` fallback.
- **split-point 테스트**: 유효 요청의 모든 prefix가 `HTTP_PARSE_INCOMPLETE`인지 확인.
- **검증**: 단위 123 assertions, integration 98 PASS, sanitize PASS, fd PASS.

### Phase 2로 넘긴 것

- total response deadline / 최소 처리량 정책 (slow-drip 클라이언트)
- non-blocking write + `POLLOUT` (이벤트 루프 선택 시)
- `--send-timeout` 별도 설정
- 32비트 large-file 산술 (`off_t`/`size_t`)
- `If-Range`, `ETag`, `Date`, `Cache-Control`, `X-Content-Type-Options`
- `sendfile`/`writev`/`TCP_NODELAY` 최적화
- hard link, TOCTOU, FIFO open
- `ELOOP`/`EMFILE`/`ENAMETOOLONG` 상태 매핑

## ASan/UBSan 메모

- macOS의 ASan은 `detect_leaks=1`을 지원하지 않는다. (`make sanitize`에서 `detect_leaks`를 빼고 `halt_on_error=1`만 사용)
- macOS에서 메모리 누수는 `leaks`/Instruments로 확인한다.
- Linux에서는 `ASAN_OPTIONS=detect_leaks=1` 또는 valgrind `--leak-check=full`을 사용할 수 있다.
- `make sanitize`는 다음 플래그를 사용한다.
  - `-fsanitize=address,undefined`
  - `-fno-omit-frame-pointer`
  - `-MMD -MP`

## 부하 테스트 메모

- Phase 1은 단일 스레드라서 `-c 1`이 기준이다.
- `ab -n 1000 -c 1` (close)와 `ab -k -n 1000 -c 1` (keep-alive)을 측정했다.
- Phase 2에서 동시성을 넣은 뒤 같은 명령으로 비교한다.
- `ab`가 없으면 `brew install httpd`, `wrk`, `hey`를 사용한다.

## 알려진 한계 / 후속 작업 (2026-11-24)

Phase 1에서 의도적으로 남긴 것과 Phase 2에서 정리할 것.

### Phase 1 한계

- **access log 조기 에러**: 헤더가 완성되기 전의 `408`/`414`/`431`은 access log에 남지 않는다. 현재는 파싱된 요청만 로깅한다. 모든 요청을 로깅하려면 헤더 읽기 시작 시점에 log context를 열고 method를 prefix에서 감지해야 한다.
- **CGI PATH**: `build_cgi_env`는 `PATH=/usr/bin:/bin`만 넘긴다. C CGI는 문제없지만 `#!/usr/bin/env python3` 같은 스크립트는 macOS의 `/usr/local/bin`/`/opt/homebrew/bin`이 없어 실패할 수 있다. 현재 지원 범위는 C CGI.
- **CGI 실행 권한 검사**: `access(X_OK)` 대신 mode bit(`S_IXUSR|S_IXGRP|S_IXOTH`)만 본다. 서버 사용자 기준 effective 실행 권한과 다를 수 있어 `execve` 실패 시 `500`이 될 수 있다. `faccessat(..., AT_EACCESS)`로 개선 가능.
- **macOS CGI 실행 race**: Linux는 `fexecve(script_fd)`지만 macOS는 `close(script_fd); execve(resolved)` fallback이라 최종 파일 교체 race가 남는다. Phase 2 `posix_spawn`/플랫폼별 fd 실행으로 정리한다.

### Phase 2에서 정리

- **access log logger**: `access_log_end`의 `fprintf`가 블로킹이고, `g_current`가 thread-local 포인터라 이벤트 루프에서 요청이 interleave되면 로그가 섞일 수 있다. per-connection log + logger thread/bounded queue로 전환한다.
- **lingering close**: `lingering_close`가 최대 1초 blocking이다. 이벤트 루프에서는 non-blocking `POLLIN` + deadline 상태로 바꾼다.
- **early header error logging**: 위 Phase 1 한계와 같은 항목. Phase 2에서 헤더 read 시작 시 log context를 연다.
- 상세: [phase2-design.md](phase2-design.md) §15.

## 남은 품질 항목

- README 작성 (사용자가 나중에 한 번에 작성)
- Linux 빌드/테스트 (`make linux-test` 또는 GitHub Actions CI)
- CGI 큰 POST 교착 테스트
- CGI fd 상속 테스트
- bind 실패/EMFILE 같은 운영 엣지 테스트

## 통과 기준

- `make test`/`make integration` 실패 0
- ASan/UBSan 리포트 0
- fd 수가 요청 전후로 안정
- `ab` 실패 요청 0
- macOS + Linux 빌드
- 위 항목을 README/이 문서에 기록
