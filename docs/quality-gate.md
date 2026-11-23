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
| 단위 테스트 | `make test` | **73 assertions, 0 failed** |
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

### Phase 2로 넘긴 것

- total response deadline / 최소 처리량 정책 (slow-drip 클라이언트)
- non-blocking write + `POLLOUT` (이벤트 루프 선택 시)
- `--send-timeout` 별도 설정
- 32비트 large-file 산술 (`off_t`/`size_t`)
- `If-Range`, `ETag`, `Date`, `Cache-Control`, `X-Content-Type-Options`
- `sendfile`/`writev`/`TCP_NODELAY` 최적화
- hard link, TOCTOU, FIFO open, `Location` CRLF 주입
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
