# Phase 2 요구사항 — 동시성

> 상위 인덱스: [requirements.md](requirements.md)
> Phase 1의 단일 연결 서버를 여러 클라이언트가 동시에 사용할 수 있게 확장한다.

## 0. 범위

- 동시 요청 처리: `fork()` / `pthread` 스레드 풀 / I/O 멀티플렉싱(`select`/`poll`/`epoll`/`kqueue`) 중 **1개 이상**.
- 동시 다중 클라이언트 접속.
- keep-alive는 Phase 1 후반(D9=B+)에서 이미 구현했으므로, Phase 2에서는 동시성 아래에서 유지/튜닝한다.
- 타임아웃, backpressure, graceful shutdown.
- 부하 테스트(`ab`, `wrk`, `hey`).
- 제외: 프록시, TLS, HTTP/2.

## 1. 결정 요약

| ID | 결정 | 상태 | 이유 |
|---|---|---|---|
| D2 | 동시성 모델 | **미확정** (추천: pthread 스레드 풀) | 구현 단순/이식성. 이벤트 루프는 C10K/난이도, fork는 단순/비용 |
| D9 | keep-alive | Phase 1에서 구현, Phase 2에서 유지/튜닝 | 연결별 상태를 동시성 모델에 맞게 관리 |
| D13 | 테스트 전략 | Phase 1 결정을 따름 | 동시성 테스트 추가 |

### D2 선택지

| 방안 | 장점 | 단점 | 적합 |
|---|---|---|---|
| `fork` | 구현 단순, 프로세스 격리, 크래시 전파 적음, 락 불필요 | 프로세스 생성 비용, 메모리 중복, C10K에 불리 | 연결 수가 적고 단순성/안정성 우선 |
| pthread 스레드 풀 | 균형, 이식성, 스레드 재사용, 공유 상태 관리 가능 | 락/스레드 안전성, 느린 CGI가 워커 점유, 시그널 복잡 | 이해 가능한 동시 서버 |
| I/O 멀티플렉싱 | C10K, 메모리 효율, 포트폴리오 임팩트 | 상태 머신, non-blocking, CGI 결합 어려움, 플랫폼 API | 성능/임팩트 최대, 시간 충분 |

## 2. 기능 요구사항

| ID | 요구사항 | 우선순위 | 검증 |
|---|---|---|---|
| FR-13 | fork / pthread / I/O 멀티플렉싱 중 1개 이상 | Must | 동시 접속 테스트 |
| FR-15 | I/O 멀티플렉싱 선택 시 이벤트 루프 | 조건부 Must | 다중 fd 테스트 |
| FR-14 | keep-alive를 동시성 모델 아래에서 유지/튜닝 | Must | 같은 연결 2+ 요청, 동시 다중 클라이언트 |
| FR-29 | 연결 수 제한/backpressure 정책 | Should | 한계 초과 접속 테스트 |
| FR-30 | SIGTERM 시 활성 연결 정리 | Must | 종료 중 요청 처리 |
| FR-31 | 동시성 모델별 타임아웃 정책 | Must | 느린 클라이언트 테스트 |

### 상세

- **FR-13**
  - 선택한 모델의 구현을 완성한다.
  - `fork`: 부모는 `accept`, 자식은 연결 처리. 자식 회수(`SIGCHLD`/`waitpid`).
  - pthread: 요청 처리 스레드 풀. 큐, 워커 수, 종료 플래그.
  - 멀티플렉싱: non-blocking 소켓, 이벤트 루프, 연결별 상태 머신.
- **FR-14**
  - Phase 1의 `should_close`/버퍼 잔여 처리 유지.
  - 연결별 상태(버퍼, 요청 수, 마지막 활동 시각)를 동시성 모델에 맞게 관리.
  - keep-alive가 다른 연결을 굶기지 않도록 타임아웃/공정성 확보.
- **FR-29**
  - 최대 동시 연결 수 또는 최대 워커 수를 설정.
  - 초과 시 `503` 또는 연결 종료/백오프 정책 결정.
- **FR-30**
  - SIGTERM/SIGINT 시 새 연결 중단, 활성 연결은 제한 시간 내 마무리.
  - 강제 종료 시 fd/메모리 정리.
  - **Phase 1 현황**: 단일 스레드라 요청 처리 중 `listenfd`가 열려 있어 backlog 연결이 RST될 수 있다. keep-alive는 `g_stop`으로 끊고, 두 번째 시그널은 `_exit`로 강제 종료한다.
  - **Phase 2**: accept 담당 주체가 `listenfd`를 먼저 닫고, 활성 연결/워커를 추적해 유예 시간 후 종료한다.
- **FR-31**
  - 느린 클라이언트, 느린 CGI, 느린 백엔드(추후)에 대한 타임아웃.
  - Phase 1의 요청 타임아웃/keep-alive idle을 동시성 모델에 맞게 조정.

## 3. 비기능 요구사항

| ID | 요구사항 | 검증 |
|---|---|---|
| NFR-07 | process-per-request vs thread pool vs event loop, C10K 트레이드오프 문서화 | 설계 문서 |
| NFR-09 | 동시성 상황에서도 malloc/free, fd 회수 | ASan/TSan |
| NFR-10 | SIGINT/SIGTERM 안전 종료, SIGPIPE 무시 | 시그널 테스트 |
| NFR-13 | `ab`, `wrk`, `hey`로 처리량/지연 측정 | 결과 리포트 |
| NFR-14 | valgrind/sanitizer, 가능하면 ThreadSanitizer | 클린 로그 |
| NFR-15 | 동시 접속 중 클라이언트 끊김, 느린 클라이언트 | 테스트 케이스 |

## 4. 테스트 / 인수 기준

- [ ] 동시 클라이언트 N개 접속 시 정상 응답 (모델에 따라 10/100/1000)
- [ ] keep-alive 연결이 동시성 아래에서도 정상 동작
- [ ] 느린 클라이언트가 다른 클라이언트를 굶기지 않음 (또는 정책 문서화)
- [ ] SIGTERM 시 활성 연결 정리 후 종료
- [ ] 연결 수 제한 초과 시 정해진 정책대로 동작
- [ ] `ab -n 10000 -c 100`, `wrk` 등 부하 테스트 결과 기록
- [ ] ASan/UBSan 클린, 가능하면 TSan/helgrind
- [ ] 스레드/프로세스/이벤트 루프 누수 없음
- [ ] README에 동시성 모델과 트레이드오프 문서화

## 5. 범위 제외

- Proxy/Reverse Proxy (Phase 3)
- TLS, HTTP/2, HTTP/3
- HTTP pipelining
- 고급 스케줄링/우선순위 큐 (선택)

## 6. Phase 1에서 넘어오는 것

- `accept` 루프 + `should_close`.
- 연결별 버퍼/요청 수/마지막 활동 시각.
- 파서/응답/정적/CGI 모듈.
- keep-alive 동작과 테스트.
- 시그널 처리 뼈대.

## 7. 열린 결정

- **D2**: fork / pthread 스레드 풀 / I/O 멀티플렉싱.
- 스레드 풀 크기, 연결 수 제한, 타임아웃 값.
- 이벤트 루프 선택 시 `poll` vs `kqueue`/`epoll` (`#ifdef`).
- 부하 테스트 도구(`ab`/`wrk`/`hey`)와 목표 수치.

## 8. 동시성 구현 메모

### 8.1 `FD_CLOEXEC`와 멀티스레드 경쟁

- `open()`/`socket()` 후 `fcntl(fd, F_SETFD, FD_CLOEXEC)`를 호출하는 방식은 **원자적이지 않다.**
- 두 호출 사이에 다른 스레드가 `fork()` + `execve()`를 하면, 그 짧은 구간에 fd가 자식에게 상속될 수 있다.
- 가능하면 **생성 시점에 원자적으로** 설정한다.
  - `open(path, O_CLOEXEC)`
  - `pipe2(fds, O_CLOEXEC)` (Linux)
  - `accept4(fd, ..., SOCK_CLOEXEC)` (Linux)
- macOS에는 `accept4()`가 없어 `accept()` 후 `fcntl()`을 쓰게 된다. 이 경쟁을 없애려면 CGI 자식에서 `execve()` 전에 **필요한 fd만 남기고 나머지를 닫는다.**
  - macOS/BSD: `closefrom(3)`
  - Linux: `close_range(3, ~0U, 0)` (5.9+) 또는 `/dev/fd` 순회
- `dup2()`로 만든 새 fd는 `FD_CLOEXEC`가 **꺼진 상태**가 된다. CGI 자식의 stdin/stdout은 exec 후에도 살아 있어야 하므로 이 동작을 활용한다.
- Phase 1의 `fcntl(listenfd, F_SETFD, FD_CLOEXEC)`는 단일 스레드라 안전하다. Phase 2에서 스레드를 도입하면 위 경쟁을 함께 설계해야 한다.

### 8.2 멀티스레드 `fork`와 async-signal-safe

- Phase 1은 단일 스레드라 `fork` 후 `setenv`/`malloc`/`printf`가 문제되지 않는다.
- Phase 2에서 스레드 풀을 쓰면 `fork()`는 **워커 스레드**에서 호출된다. 이때:
  - 자식에는 **`fork()`를 호출한 스레드만** 복사된다.
  - 메모리는 fork 시점의 스냅샷(COW)이라, 다른 스레드가 잡고 있던 뮤텍스/`malloc`/`stdio`/`environ` 락이 **잠긴 상태로** 남는다.
  - 락을 풀어줄 스레드가 자식에 없으므로, 자식이 그 락을 잡으려 하면 **영원히 대기**한다.
- POSIX 규칙: 멀티스레드 프로세스에서 `fork()`한 자식은 `execve()` 전까지 **async-signal-safe 함수만** 호출할 수 있다.
  - 안전: `execve`, `_exit`, `dup2`, `close`, `read`, `write`, `open`, `fcntl`, `pipe`, `waitpid`, `kill`
  - 위험: `malloc`/`free`, `printf`/`fprintf`/`snprintf`, `setenv`/`putenv`/`getenv`, `strerror`, `exit`
- `pthread_atfork`로 우리 코드의 락은 정리할 수 있지만, `malloc`/`stdio`/`environ` 같은 **libc 내부 락**은 해결하지 못한다.
- 현재 `cgi.c`의 문제:
  - 자식에서 `set_cgi_env()`가 `setenv()`를 호출한다. → 멀티스레드 fork 후 unsafe.
- 향후 `envp`를 부모에서 만드는 방식으로 바꿀 때:
  - 여러 워커가 `setenv()`를 공유하면 `environ` 레이스가 생기므로, 스레드별 `envp` 배열을 써야 한다.
- Phase 2 리팩터링:
  1. **fork 전에 부모에서 `envp` 배열을 만든다.** `malloc`/`strdup`은 부모에서 정상 락으로 안전하다.
  2. 자식은 `dup2`/`close`/`execve`/`_exit`만 호출한다.
  3. 또는 `posix_spawn()` + `posix_spawn_file_actions_*`로 fork+exec를 라이브러리에 맡긴다.
- 증상: 부하가 걸릴 때만 CGI가 간헐적으로 멈추고 `504`가 난다. `gdb -p`/`sample`로 보면 자식이 `__lll_lock_wait`, `malloc`, `setenv` 등에서 대기한다.
- 결론: **스레드 풀 도입 전에 `cgi.c`의 `setenv`를 `envp` 기반으로 바꾸는 작업이 선행되어야 한다.**
