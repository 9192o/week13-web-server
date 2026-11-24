# Phase 2 설계 — 이벤트 루프 + 작업 스레드 풀

> 상위: [requirements.md](requirements.md), [phase2-requirements.md](phase2-requirements.md)
> D2 변경: **pthread 연결 워커 풀 → 단일 이벤트 루프 + 작업 스레드 풀**
> 이유: 많은 연결/keep-alive를 fd 단위로 관리하고, CGI·디스크 같은 블로킹 작업만 스레드 풀로 위임한다.

## 0. 목표

- 소켓 I/O와 HTTP 상태 관리는 **논블로킹 이벤트 루프**가 맡는다.
- CGI/디스크 같은 **블로킹 작업**은 작은 작업 스레드 풀이 맡는다.
- 이벤트 루프는 **절대 블로킹하지 않는다.**
- keep-alive idle 연결을 fd 하나로 관리한다.
- 과부하 시 503으로 빠르게 실패한다.
- SIGINT/SIGTERM에서 진행 중인 요청을 마치고 종료한다.

## 1. 확정 결정

| 항목 | 결정 | 비고 |
|---|---|---|
| 이벤트 루프 | **1개, 메인 스레드** | multi-reactor는 후속 |
| 이벤트 백엔드 | **epoll(Linux) / kqueue(macOS) / poll(폴백)** | `event_backend`로 추상화. Phase 2는 level-triggered |
| 소켓 | **논블로킹** | `O_NONBLOCK` |
| 작업 스레드 풀 | **CGI/디스크용**, 기본 **4 workers** | `--task-workers` |
| 작업 큐 | **64** 기본, bounded | `--task-queue` |
| 최대 연결 | **1024** 기본 | `--max-conns` |
| keep-alive | idle **5초**, max **100** | Phase 2 튜닝 |
| 정적 파일 | **이벤트 루프 (A)**, MVP | 대안 B/C/D는 §7.1. 데모 페이지 성격/부하 테스트에 따라 재검토 |
| 과부하 | 작업 큐 full / 연결 상한 → **503** | `Retry-After: 1` |
| CGI | 작업 풀에서 **`posix_spawn` + `envp`** | 결과를 버퍼로 반환 |
| 종료 | self-pipe + drain + **5초 유예** | 강제 종료 watchdog |
| 시그널 | 메인 루프만 처리, 작업 워커는 블록 | SIGPIPE 무시 |
| 로그 | **단일 로거 스레드 + bounded 큐** | full이면 drop |

> 4 workers / 64 queue / 1024 conns는 **제안 기본값**이다. 부하 테스트에서 조정한다.

## 2. 전체 구조

```text
main thread = event loop
  ├─ listenfd (POLLIN)
  ├─ connection fds (POLLIN/POLLOUT)
  ├─ completion pipe (POLLIN)  ← 작업 완료 통지
  ├─ signal self-pipe (POLLIN)
  └─ timers (idle/request deadlines)
        │
        ├─ HTTP 파싱/상태 전이/응답 쓰기
        ├─ 정적 파일 서빙
        └─ CGI 필요 → task queue (bounded 64)
                          │
                    [task workers 4]
                          │
                    completion queue + pipe
                          │
                    event loop가 결과를 연결에 붙임

logger thread
  └─ log queue 1024 → stderr
```

## 3. 이벤트 루프

```c
for (;;) {
    int timeout = next_deadline_ms();

    ev_event_t events[64];
    int n = ev_wait(loop, events, 64, timeout);

    for (int i = 0; i < n; i++) {
        int fd = events[i].fd;
        uint32_t ev = events[i].events;
        void *ptr = events[i].ptr;

        if (fd == listenfd && (ev & EV_READ))       accept_new_connections();
        if (fd == completion_pipe && (ev & EV_READ)) drain_task_completions();
        if (fd == signal_pipe && (ev & EV_READ))     handle_shutdown_signal();
        if (ptr != NULL && (ev & EV_READ))           on_readable(ptr);
        if (ptr != NULL && (ev & EV_WRITE))          on_writable(ptr);
        if (ev & EV_ERR)                             close_conn(ptr);
    }

    expire_timers();
}
```

- **블로킹 호출 금지**: `read`/`write`는 `EAGAIN`까지만, CGI/디스크는 작업 풀로.
- `ev_wait` timeout은 **가장 가까운 deadline**을 계산해 넘긴다.
- `accept`는 논블로킹으로 `EAGAIN`까지 반복.
- `listenfd`는 `FD_CLOEXEC`.
- 백엔드는 `epoll`(Linux), `kqueue`(macOS), `poll`(폴백)을 같은 인터페이스로 감싼다.
- Phase 2는 **level-triggered**만 쓴다. `EPOLLET`/`EV_CLEAR`는 후속.

## 4. 연결 상태 머신

```c
enum conn_state {
    ST_READ_HEADERS,
    ST_READ_BODY,
    ST_PROCESS_STATIC,
    ST_WAIT_TASK,
    ST_WRITE,
    ST_CLOSING
};
```

| 상태 | 하는 일 | 다음 |
|---|---|---|
| `READ_HEADERS` | `\r\n\r\n`까지 읽기 | 헤더 완료 → `READ_BODY`/`PROCESS_STATIC`/`WAIT_TASK` |
| `READ_BODY` | `Content-Length`만큼 누적 | 완료 → 처리 |
| `PROCESS_STATIC` | 파일 열기/헤더 준비 | `WRITE` |
| `WAIT_TASK` | CGI 작업 완료 대기 | 완료 → `WRITE` |
| `WRITE` | 출력 버퍼/파일 쓰기 | 완료 → keep-alive or `CLOSING` |
| `CLOSING` | fd 정리 | 제거 |

연결 컨텍스트:

```c
typedef struct conn {
    int fd;
    struct sockaddr_in peer;
    enum conn_state state;

    char in[16384];
    size_t in_len;

    char *out;
    size_t out_len, out_sent;

    http_request_t req;
    size_t body_received;

    time_t deadline;
    int requests;
    uint64_t id;          /* task 완료 매칭 */
} conn_t;
```

- 이벤트 루프가 `conn_t`를 소유한다. 작업 워커는 `conn_t`/`fd`를 건드리지 않는다.
- `id`로 완료 결과가 **아직 살아있는 연결**인지 확인한다.

## 5. 읽기/파싱/본문

- 헤더는 `in` 버퍼에 누적하고 `\r\n\r\n`을 찾으면 기존 `http_request_parse_buffer`를 호출한다.
- 본문은 `Content-Length`만큼 **논블로킹으로 누적**한다.
  - 기존 `http_request_read_body`는 `readn`으로 블로킹하므로 이벤트 루프에서 쓰지 않는다.
  - `READ_BODY` 상태에서 `read` → `EAGAIN`이면 대기.
- 본문 최대 8MB. 초과 시 413.
- 헤더 최대 16KB. 초과 시 431.
- keep-alive 잔여 바이트는 다음 요청으로 넘긴다.
- 에러 시 에러 응답을 버퍼에 넣고 `Connection: close`.

## 6. 쓰기/부분 쓰기

- `WRITE` 상태에서 `write`를 시도한다.
- `EAGAIN`이면 남은 바이트를 유지하고 `POLLOUT` 등록.
- 정적 파일은 헤더를 보낸 뒤 파일을 청크로 읽어 쓴다.
  - `sendfile`은 후속 최적화. MVP는 `read`/`write` 루프.
  - 디스크 읽기는 이벤트 루프에서 블로킹될 수 있다. 큰 파일/느린 디스크는 후속 과제.
- `EPIPE`/`ECONNRESET`이면 연결을 닫는다.
- 출력 버퍼 상한을 두고, 넘으면 연결을 닫는다.

## 7. 정적 파일

- 기존 `static_file.c`의 **정책/보안 검사**는 재사용한다.
- `static_file_handle`을 두 부분으로 나눈다.
  1. `static_file_prepare`: 경로 검증, `stat`/`open`, MIME, `Last-Modified`, Range, 헤더 준비.
  2. 이벤트 루프가 헤더를 쓰고 파일 본문을 청크로 전송.
- `HEAD`는 헤더만.
- 304/416/301은 기존 로직 재사용.
- 파일 fd는 연결이 닫힐 때 정리.

### 7.1 결정: A — 이벤트 루프 (MVP)

Phase 2 MVP는 **정적 파일을 이벤트 루프에서 처리**한다. `static_file_prepare`가 정책/보안/헤더를 준비하고, 루프가 파일을 64KB 청크로 읽어 non-blocking write 한다.

**선택 이유**

- 로컬 파일은 page cache에 있어 `read`가 빠르고, Phase 1 `static_file.c`의 경로/보안/캐시/Range 로직을 거의 그대로 재사용한다.
- CGI 작업자 4개를 정적 파일이 점유하지 않아 "느린 CGI + 정적 요청"이 자연스럽게 통과한다.
- 구현이 가장 빠르고, Phase 1 통합 테스트를 거의 그대로 쓸 수 있다.

**대안**

| 대안 | 장점 | 단점 | MVP |
|---|---|---|---|
| A. 이벤트 루프 | 단순, Phase 1 재사용, 작은 파일 빠름, CGI 워커 비점유 | 디스크 read가 루프를 막을 수 있음 | **채택** |
| B. 작업 풀 | 루프 비블로킹, 느린 디스크/큰 파일에 안전 | 버퍼/큐/복사/취소/순서 관리 복잡, CGI와 큐 공유 시 starvation | 보류 |
| C. `sendfile` | zero-copy, 큰 파일 처리량/CPU 유리 | 플랫폼별 API, partial send, 디스크 read는 여전히 블로킹 가능 | 후속 최적화 |
| D. 하이브리드 | 작은 파일은 루프, 큰 파일은 sendfile/풀 | 경로 2개, 임계값 튜닝, 테스트 표면 증가 | 필요 시 |

**재검토 트리거**

- 데모 페이지가 큰 이미지/동영상/파일 다운로드 위주이거나 동시 다운로드가 많으면 **C/D**를 검토한다.
- 부하 테스트에서 정적 파일 서빙 중 다른 연결의 지연이 튀면(이벤트 루프 블로킹) **C/D**로 승격한다.
- B는 디스크가 실제로 느린 환경(네트워크 FS 등)에서 A/C로 해결되지 않을 때만 검토한다.

## 8. CGI + 작업 스레드 풀

### 8.1 작업

```c
typedef struct {
    uint64_t conn_id;
    char method[16];
    char path[PATH_MAX];
    char query[HTTP_MAX_URI];
    char *body;
    size_t body_len;
    /* CONTENT_TYPE 등 필요한 헤더 */
} cgi_job_t;

typedef struct {
    uint64_t conn_id;
    char *response;     /* 직렬화된 HTTP 응답 */
    size_t response_len;
    int close_connection;
} cgi_result_t;
```

- 이벤트 루프가 요청을 복사해 작업 큐에 넣는다.
- 큐가 가득 차면 **503**.
- 작업 워커는 `posix_spawn`으로 CGI를 실행하고 (S2c 목표; S2b는 `fork`+`execve`), 출력/타임아웃/크래시를 처리해 **응답 버퍼**를 만든다.
- 완료되면 결과 큐에 넣고 **completion pipe에 1바이트** 쓴다.
- 이벤트 루프가 결과를 꺼내 `conn_id`로 연결을 찾고, `out`에 붙인 뒤 `WRITE`로 전이.
- 연결이 이미 닫혔으면 결과를 버린다.
- **작업 워커는 `connfd`를 쓰지 않는다.** 소켓 I/O는 항상 이벤트 루프만 한다.

### 8.2 CGI 실행

- **S2b 현재 구현**: 작업 워커가 `fork` + `execve`로 CGI를 실행하고, Phase 1 `cgi_handle`의 출력은 임시 파일에 캡처해 응답 버퍼로 만든다. `posix_spawn` 전환은 S2c.
- **macOS signal mask**: macOS는 signal mask를 프로세스 단위로 공유하고 `fork()`가 그 주변에서 signal을 블록한다. 동시 fork가 겹치면 SIGINT/SIGTERM이 영구히 블록될 수 있으므로, `cgi.c`의 fork를 뮤텍스로 직렬화하고 이벤트 루프가 `ev_wait` 전에 shutdown signal을 unblock한다.
- stdin은 POST 본문 임시 파일, stdout은 파이프.
- `poll`로 출력 대기, `request_timeout_sec` 초과 시 `SIGKILL` → 504.
- 출력 1MB 제한, 0바이트/크래시 → 500.
- CGI 헤더 파싱, `Status:`, `Content-Type` 기본값, `Content-Length` 재계산은 기존 로직 재사용.
- `waitpid`로 회수. 좀비 금지.

### 8.3 정적 파일과 CGI의 차이

- 정적 파일: **이벤트 루프에서 처리 (A, §7.1)**. 디스크 read가 루프를 막을 수 있다.
- CGI: 작업 풀에서 처리(블로킹 허용).
- 디스크 I/O가 병목이면 후속으로 C(`sendfile`) 또는 D(하이브리드)를 검토한다. B(작업 풀)는 큐/메모리 복잡도 때문에 최후 수단.

## 9. keep-alive와 타이머

- idle 5초, max requests 100.
- 연결마다 `deadline`을 둔다.
  - `READ_HEADERS`에서 `in_len == 0`: idle deadline.
  - 요청이 시작됨: request deadline.
- `ev_wait` timeout은 가장 가까운 deadline.
- 응답 완료 후 keep-alive면 상태를 리셋하고 `POLLIN` 유지.
- idle 만료 시 조용히 close.
- **이벤트 루프에서는 idle 연결이 fd + 버퍼만 차지**하므로 스레드 풀보다 효율적이다.

## 10. 백프레셔/503

- **최대 연결 수**: 기본 1024 (`--max-conns`). 초과 시 새 연결에 **503 + `Retry-After: 1`** 후 close. (S2c-1 구현)
- **작업 큐 full**: CGI 요청에 **503 + `Retry-After: 1`**. (S2b 구현)
- **출력 버퍼 상한**: 넘으면 close.
- 503 응답: `Connection: close`, `Retry-After: 1`.
- `HTTP_SERVICE_UNAVAILABLE = 503`과 reason은 추가됐다.

## 11. 종료/시그널

- SIGINT/SIGTERM 핸들러는 **self-pipe에 1바이트**만 쓴다 (`write`는 async-signal-safe). S1에서 구현 완료.
- 이벤트 루프가 self-pipe를 읽고 종료 절차 시작. 현재 S2b 구현:
  1. 루프 종료 후 모든 연결 close
  2. `task_pool_shutdown`으로 진행 중 작업 완료 대기
  3. 결과 큐 drain 후 풀/이벤트 루프 정리
- S2c 목표: listenfd close → 진행 중 요청 완료 → idle/read 연결 close → 새 작업 금지 → 5초 유예 → watchdog `_exit`.
- macOS는 signal mask가 프로세스 공유라 워커의 `fork()`가 SIGINT를 블록한 채 남길 수 있다. `ev_wait` 전에 SIGINT/SIGTERM을 unblock하고 fork를 직렬화한다.
- 작업 워커와 로거는 SIGINT/SIGTERM을 블록한다 (로거 도입 시).
- `SIGPIPE` 무시.
- `SIGCHLD`는 기본 동작. CGI 작업 워커가 자기 자식 `waitpid`.

## 12. 로깅

- 단일 로거 스레드 + bounded 큐(1024).
- 이벤트 루프/작업 워커는 포맷 후 큐에 넣기만.
- full이면 drop + 카운터.
- 종료 시 sentinel + join + flush.
- 접근 로그는 옵션/샘플링.

## 13. 설정/CLI

| 옵션 | 기본값 | 범위 | 필드 |
|---|---|---|---|
| `--task-workers N` | 4 | 1..64 | `task_workers` |
| `--task-queue N` | 64 | 1..65536 | `task_queue_size` |
| `--max-conns N` | 1024 | 1..65536 | `max_conns` |
| `--keepalive-idle N` | 5 | 1..3600 | `keepalive_idle_sec` |
| `--keepalive-max N` | 100 | 1..1000000 | `keepalive_max_requests` |

- `config_init`/`config_parse_cli`/`config_validate`.
- 시작 로그: `listening ... (loop=<epoll|kqueue|poll>, task_workers=4, max_conns=1024)`.

## 14. 변경 파일/선행 작업

> 구현 상태 (S0): `src/event_backend.c` + `event_poll.c` + `event_epoll.c` + `event_kqueue.c`와 `tests/test_event_backend.c` 완료. macOS에서 poll/kqueue PASS, epoll은 Linux CI에서 확인 예정.
>
> 구현 상태 (S1): `server_ev` (이벤트 루프 + 연결 상태 머신 + 정적 파일 A)와 `tests/integration_s1.sh` 완료. Phase 1 `server`는 그대로 유지하고, S2에서 `server_ev`에 작업 풀/CGI를 붙인 뒤 통합한다. S1의 CGI는 `503` 스캐폴드다.
> S1 스캐폴드 한계: signal은 `EINTR` 폴링으로 처리하고 self-pipe/drain은 아직 없다. max-conns 초과는 즉시 close(503 아님), access log/로거 스레드도 S2.
> 구현 상태 (S2a): `src/task_pool.c` + `include/task_pool.h` + `tests/test_task_pool.c` 완료. bounded queue/workers/completion pipe/queue-full/timeout 테스트 PASS. CGI job 연결은 S2b.
>
> 구현 상태 (S2b): `cgi_job_t`/`cgi_result_t` + task pool 연결 완료. CGI GET/POST/crash/10 concurrent/keep-alive/timeout 504, SIGTERM 무시 CGI 504, SIGINT 종료 테스트 PASS. `posix_spawn`, 로거 스레드, max-conns/queue-full 503은 S2c.
>
> 구현 상태 (S2c-1): `--task-workers`, `--task-queue`, `--max-conns` config/CLI 추가. max-conns 초과 503 + `Retry-After: 1`, queue-full 503. `tests/integration_s2.sh` PASS.
>
> `EV_BACKEND=poll|kqueue|epoll`로 백엔드를 강제할 수 있다.

| 파일 | 변경 |
|---|---|
| `src/server.c` | Phase 1 서버 (S2c에서 `server_ev`로 교체/통합) |
| `src/main_ev.c`, `src/server_ev.c`, `include/server_ev.h` | S1 이벤트 루프 + S2b CGI 작업 제출/완료 + S2c-1 백프레셔 |
| `src/cgi_task.c`, `include/cgi.h` | S2b CGI job/result, 임시 파일 캡처 |
| `tests/integration_s1.sh` | S1 정적 + S2b CGI/타임아웃/종료 테스트 |
| `tests/integration_s2.sh` | S2c-1 max-conns/queue-full 503 테스트 |
| `include/event_backend.h` | `ev_add`/`ev_mod`/`ev_del`/`ev_wait` 인터페이스 |
| `src/event_backend.c` | 백엔드 선택/auto fallback, 공통 API |
| `src/event_internal.h` | 백엔드 ops/state 내부 인터페이스 |
| `src/event_poll.c` | `poll` 폴백 |
| `src/event_epoll.c` | Linux `epoll` |
| `src/event_kqueue.c` | macOS/BSD `kqueue` |
| `src/task_pool.c`, `include/task_pool.h` | 작업 큐/워커/완료 파이프 (S2a 완료) |
| `src/http_request.c`, `include/http.h` | 논블로킹 본문 누적 API |
| `src/http_response.c` | 응답을 메모리로 직렬화 |
| `src/static_file.c` | 정책/prepare와 I/O 분리 |
| `src/cgi.c` | `cgi_execute`가 응답 버퍼 반환, `posix_spawn` |
| `src/config.c`, `include/config.h` | task/conns 옵션 |
| `src/util.c`, `include/util.h` | 로거 스레드 |
| `Makefile` | `-pthread` |
| `tests/` | 이벤트 루프/작업 풀 테스트 |

선행 작업:

1. `HTTP_SERVICE_UNAVAILABLE = 503` + reason은 완료. 503 응답 path + `Retry-After` 연결이 남음.
2. 응답을 fd가 아니라 **메모리 버퍼로 직렬화**하는 API.
3. `http_request`의 **논블로킹 본문** API.
4. `cgi.c` → `posix_spawn` + `envp`, `cgi_execute`가 버퍼 반환.
5. `g_stop`/종료 플래그를 `atomic_bool`.
6. 로거 스레드.
7. `Makefile` `-pthread`.

## 15. 열린 결정

- `EPOLLET`/`EV_CLEAR` edge-triggered 도입 여부 (Phase 2는 level만).
- 백엔드별 타이머 최적화(`timerfd`/`EVFILT_TIMER`) 도입 여부.
- task workers 4 vs 8.
- ~~정적 파일을 이벤트 루프에서 처리할지, 작업 풀/`sendfile`로 위임할지.~~ → **결정: A (이벤트 루프, §7.1)**. 데모 페이지/부하 테스트에 따라 C/D 재검토.
- 최대 연결 1024가 적절한지.
- 연결 상한 초과 시 503 vs accept 중단.

### Phase 1.5 자산의 Phase 2 전환

- **access log**: `access_log_t`를 요청별로 넘기도록 바꿨지만, `access_log_end`의 `fprintf`는 블로킹이다. 또한 `g_current`가 thread-local 포인터라 이벤트 루프에서 요청이 interleave되면 로그가 섞일 수 있다. Phase 2에서는 per-connection log context + 로거 큐/스레드로 바꾼다.
- **lingering close**: 현재 `lingering_close`는 최대 1초 blocking이다. Phase 2 이벤트 루프에서는 non-blocking `POLLIN` + deadline 상태로 바꿔야 한다.
- **CGI 실행**: Linux는 `fexecve`로 fd 기반 실행, macOS는 `execve` fallback이라 최종 파일 교체 race가 남는다. Phase 2 `posix_spawn`/플랫폼별 fd 실행으로 정리한다. Phase 1은 실행 권한을 mode bit로 검사하므로 effective `X_OK`와 다를 수 있고, `PATH=/usr/bin:/bin`만 넘긴다.
- **early header error**: 파싱 전 408/414/431은 아직 access log에 남지 않는다. 필요하면 헤더 읽기 시작 시점에 log context를 열고 method를 prefix에서 감지해야 한다.

## 16. 완료 기준

- FR-13: 이벤트 루프 + 작업 풀 동시 처리.
- FR-14: keep-alive가 이벤트 루프 아래에서 동작.
- FR-29: 연결/작업 큐 상한 초과 시 503.
- FR-30: SIGTERM/SIGINT drain 후 종료.
- FR-31: 느린 클라이언트/CGI 타임아웃.
- Phase 1 통합 테스트 전체 PASS.
- ASan/UBSan 클린, 가능하면 TSan 클린.
- fd/메모리/좀비 누수 없음.
- `ab -c 1/10/100` 결과 기록.
