# Phase 2 테스트 계획 — 이벤트 루프 + 작업 스레드 풀

> 상위: [phase2-requirements.md](phase2-requirements.md), [phase2-design.md](phase2-design.md)
> 원칙: **테스트 케이스를 먼저 쓰고, 구현이 맞춘다.**

## 0. 테스트 계층

| 계층 | 도구 | 대상 |
|---|---|---|
| 단위 | `run_tests` (C harness) | 설정 파싱, 503 reason, 큐/상태 전이 |
| 통합 | `tests/*.sh` + `curl`/`nc` | 이벤트 루프, partial I/O, keep-alive, CGI, 종료 |
| 스트레스 | `ab`, 반복 실행 | 레이스, fd 누수, 좀비 |
| 동적 | ASan/UBSan/TSan | 메모리/스레드 오류 |

## 1. 설정/CLI

| ID | 요구사항 | 유형 | 시나리오 | 기대 |
|---|---|---|---|---|
| CFG-01 | FR-29 | 단위 | `--task-workers 0` | usage/exit 1 |
| CFG-02 | FR-29 | 단위 | `--task-queue 0` | usage/exit 1 |
| CFG-03 | FR-29 | 단위 | `--max-conns 0` | usage/exit 1 |
| CFG-04 | FR-29 | 통합 | 옵션 생략 | 기본 4/64/1024로 시작 |
| CFG-05 | FR-29 | 통합 | `--keepalive-idle 1` | 1초 idle close |

## 2. 이벤트 루프 기본

| ID | 요구사항 | 유형 | 시나리오 | 기대 |
|---|---|---|---|---|
| EVENT-01 | FR-13 | 통합 | 헤더를 1바이트씩 나눠 전송 | 200, 이벤트 루프가 다른 요청도 처리 |
| EVENT-02 | FR-13 | 통합 | 동시에 100개 `GET /index.html` | 전부 200, 본문 교차 없음 |
| EVENT-03 | FR-13 | 통합 | 큰 파일을 느린 클라이언트로 수신 | partial write 처리, 본문 길이 일치 |
| EVENT-04 | FR-13 | 통합 | 느린 클라이언트 2명 + 정상 요청 1명 | 정상 요청이 지연되지 않음 |
| EVENT-05 | FR-31 | 통합 | 헤더를 보내다 5초 멈춤 | 408, 다른 연결은 계속 |
| EVENT-06 | FR-13 | 통합 | 200개 idle keep-alive 연결 유지 | 정상 요청이 계속 처리됨 |
| EVENT-07 | FR-13 | 통합 | `GET` + `HEAD` + `Range` 혼합 | 상태/본문 정확 |

### 2.1 이벤트 백엔드

| ID | 요구사항 | 유형 | 시나리오 | 기대 |
|---|---|---|---|---|
| BACKEND-01 | FR-13 | 통합 | `poll` 폴백 백엔드 | EVENT-01~07 통과 |
| BACKEND-02 | FR-13 | 통합 | Linux `epoll` 백엔드 | EVENT-01~07 통과 |
| BACKEND-03 | FR-13 | 통합 | macOS `kqueue` 백엔드 | EVENT-01~07 통과 |
| BACKEND-04 | FR-13 | 통합 | 같은 요청 시퀀스를 백엔드별로 실행 | 상태/헤더/본문 동일 |
| BACKEND-05 | FR-13 | 통합 | level-triggered에서 partial read | 다음 이벤트에서 나머지 읽기 |

> 구현 상태 (S0): `tests/test_event_backend.c`에서 `poll`과 native(`epoll`/`kqueue`)의 add/mod/del/wait/timeout/에러를 검증. macOS poll/kqueue PASS, Linux epoll은 CI에서 확인 예정.
>
> 구현 상태 (S1): `tests/integration_s1.sh` — 정적/HEAD/Range/304/OPTIONS/keep-alive/20 concurrent/느린 헤더/431/414 PASS. S1 `server_ev`의 CGI는 S2 전까지 `503` + `Retry-After: 1`.
>
> 구현 상태 (S2a): `tests/test_task_pool.c` — bounded queue/workers/completion pipe/queue-full/timeout PASS. CGI job 연결은 S2b.
>
> 구현 상태 (S2b): `tests/integration_s1.sh` — CGI GET/POST/crash/10 concurrent/keep-alive, `--timeout 1`에서 `sleep`/SIGTERM 무시 CGI 504, CGI 타임아웃 후 서버 생존, SIGINT 종료 PASS. 큐 full 503/max-conns 503은 S2c.

## 3. 연결 상한/백프레셔

| ID | 요구사항 | 유형 | 시나리오 | 기대 |
|---|---|---|---|---|
| CONN-01 | FR-29 | 통합 | `--max-conns 2` + 3번째 연결 | 3번째가 503 |
| CONN-02 | FR-29 | 통합 | 503 헤더 | `Connection: close`, `Retry-After: 1` |
| CONN-03 | FR-29 | 통합 | 503 후 서버 생존 | 연결이 줄면 다시 200 |
| TASK-01 | FR-29 | 통합 | `--task-queue 1` + 느린 CGI 다수 | 초과분 503 |
| TASK-02 | FR-29 | 통합 | 작업 큐 full 후 정적 요청 | 정적 요청은 계속 200 (루프 비블로킹) |

## 4. keep-alive

| ID | 요구사항 | 유형 | 시나리오 | 기대 |
|---|---|---|---|---|
| KEEP-01 | FR-14 | 통합 | 한 연결로 `GET` 5회 | 응답 5개, 순서 유지 |
| KEEP-02 | FR-14 | 통합 | 한 연결로 100회 | 100개 응답, 마지막 close |
| KEEP-03 | FR-14 | 통합 | 응답 후 5.5초 대기 | 서버가 close |
| KEEP-04 | FR-14 | 통합 | HTTP/1.0 + keep-alive | 유지 |
| KEEP-05 | FR-14 | 스트레스 | 50개 keep-alive 연결 × 10요청 | 교차 없음, fd 안정 |
| KEEP-06 | FR-14 | 부하 | `ab -k -n 1000 -c 32` | failed=0 |

## 5. CGI/작업 풀

| ID | 요구사항 | 유형 | 시나리오 | 기대 |
|---|---|---|---|---|
| CGI-01 | FR-13 | 통합 | 동시에 `GET /cgi-bin/echo?x=N` 20개 | 각 QUERY가 자기 값 |
| CGI-02 | FR-13 | 통합 | 동시에 `POST /cgi-bin/echo` 10개 | 각 BODY가 자기 값 |
| CGI-03 | FR-13 | 통합 | 느린 CGI 1개 + 정적 요청 1개 | **정적 요청이 즉시 응답** (루프 비블로킹) |
| CGI-04 | FR-13 | 통합 | `--task-workers 1` + 느린 CGI + 정적 요청 | 정적은 계속 응답, CGI만 대기 |
| CGI-05 | FR-12/13 | 통합 | CGI `crash` | 500 |
| CGI-06 | FR-12/13 | 통합 | CGI `sleep` + `--timeout 1` | 504 |
| CGI-07 | FR-13 | 통합 | CGI 환경변수 | METHOD/QUERY/CONTENT_LENGTH 정확 |
| CGI-08 | FR-13 | 스트레스 | CGI 100회 반복 | 좀비 없음, fd 안정 |
| CGI-09 | FR-13 | 통합 | 작업 도중 클라이언트 끊김 | 결과 폐기, 워커 생존, fd 정리 |
| CGI-10 | FR-12/13 | 통합 | SIGTERM을 무시하는 CGI + `--timeout 1` | SIGKILL 후 504, 서버 생존 |

## 6. 종료/시그널

| ID | 요구사항 | 유형 | 시나리오 | 기대 |
|---|---|---|---|---|
| SHUT-01 | FR-30 | 통합 | idle 상태 SIGINT | 5초 이내 종료 |
| SHUT-02 | FR-30 | 통합 | 요청 처리 중 SIGINT | 진행 중 요청 완료 후 종료 |
| SHUT-03 | FR-30 | 통합 | 느린 CGI 중 SIGINT | CGI 정리/504 후 종료, 5초 유예 |
| SHUT-04 | FR-30 | 통합 | SIGTERM | SIGINT와 동일 |
| SHUT-05 | FR-30 | 통합 | 종료 후 즉시 재시작 | 포트 재사용 성공 |
| SHUT-06 | FR-30 | 통합 | 두 번째 SIGINT (선택) | 즉시 강제 종료 |
| SHUT-07 | FR-30 | 스트레스 | 시작/종료 10회 | fd/스레드/좀비 누수 없음 |

> 구현 상태 (S2b): SHUT-01(idle SIGINT) PASS. SHUT-02/03/05/06/07은 S2c의 drain/watchdog 작업에서 다룬다.

## 7. 로그

| ID | 요구사항 | 유형 | 시나리오 | 기대 |
|---|---|---|---|---|
| LOG-01 | NFR-09 | 통합 | 동시 요청 100개 | 로그 줄이 섞이지 않음 |
| LOG-02 | NFR-09 | 스트레스 | 로그 큐 초과 | drop 카운터, 크래시 없음 |
| LOG-03 | FR-30 | 통합 | 종료 | flush 후 종료 |

## 8. 품질/회귀

| ID | 요구사항 | 유형 | 시나리오 | 기대 |
|---|---|---|---|---|
| REG-01 | — | 통합 | Phase 1 `make integration` | 전체 PASS |
| REG-02 | — | 단위 | Phase 1 `make test` | 전체 PASS |
| FD-01 | NFR-09 | 스트레스 | 1,000 요청 전후 fd 수 | 증가 없음 |
| MEM-01 | NFR-09 | 스트레스 | ASan/UBSan | 리포트 0 |
| RACE-01 | NFR-14 | 스트레스 | TSan + `ab -c 32` | 리포트 0 |
| LOAD-01 | NFR-13 | 부하 | `ab -c 1/10/32/100` | 실패 0, RPS/지연 기록 |

## 9. 테스트 순서

```text
1. CFG                설정 파싱/503 reason
2. BACKEND-01~05      poll → kqueue → epoll 순서로 백엔드 검증
3. EVENT-01~03        기본 루프/partial I/O
4. KEEP-01~04         keep-alive
5. CGI-01~06          CGI/작업 풀
6. EVENT-04~05        느린 클라이언트/타임아웃
7. CONN/TASK          상한/503
8. SHUT               종료
9. REG                Phase 1 회귀
10. FD/MEM/RACE       스트레스/새니타이저
11. LOAD              부하
```

## 10. 합격 기준

- 모든 테스트 PASS. 동시성/타이머 테스트는 **3회 반복**해도 PASS.
- Phase 1 `make integration` 전체 PASS.
- ASan/UBSan 클린, 가능하면 TSan 클린.
- fd 수 안정, 좀비 없음.
- `ab` 실패 0.
- 503/타임아웃/종료 정책이 문서와 일치.

## 11. 주의

- 이벤트 루프 테스트는 **partial read/write**를 반드시 포함한다. 한 번에 다 보내면 버그를 놓친다.
- 느린 CGI 테스트는 **정적 요청이 막히지 않는지**를 핵심으로 본다.
- 타이머 테스트는 `sleep`만으로 순서를 가정하지 않는다. 5초 idle은 6초 기다린다.
- **정적 파일 A 모델**: 큰 파일 + 느린 클라이언트 + 동시 작은 요청으로 이벤트 루프 지연을 측정한다. 지연이 크면 C(`sendfile`)/D(하이브리드)를 재검토한다.
- TSan은 느리므로 `-n 200 -c 8`부터.
- `ab`는 keep-alive(`-k`)와 close를 각각 측정한다.
