# Phase 1 테스트 계획 — 테스트 우선

> 상위: [phase1-requirements.md](phase1-requirements.md) · [requirements.md](requirements.md)
> 원칙: **요구사항 → 테스트 케이스 → 구현**. 실패하는 테스트를 먼저 쓰고, 통과시키는 최소 구현을 한다.

## 0. 테스트 원칙

- 모든 테스트는 FR/NFR ID와 연결한다.
- **단위 테스트**는 순수 함수(파서, URI, MIME, Range, 설정 검증)를 소켓 없이 검증한다.
- **통합 테스트**는 실제 서버를 ephemeral port로 띄우고 `curl`/`nc`로 검증한다.
- **엣지/보안 테스트**는 잘못된 요청, 한계 초과, 경로 탈출, dotfile, symlink, 끊김을 다룬다.
- **비기능 테스트**는 ASan/UBSan, fd 누수, 부하, macOS/Linux 빌드를 다룬다.
- 상태 값: `TODO`(아직 안 씀), `FAIL`(실패 확인), `PASS`, `SKIP`(Phase 1 후반/보류).

## 1. 도구 / 명령

| 레벨 | 도구 | 명령 |
|---|---|---|
| 단위 | `tests/test_*.c`, `TEST_ASSERT` | `make test` |
| 통합 | `tests/integration.sh`, `curl`, `nc` | `make integration` |
| 메모리 | ASan/UBSan | `make sanitize` |
| 부하 | `ab`/`wrk` | `make load` |
| 이식성 | macOS + Linux(Docker/CI) | `make` + `make test` |

## 2. 테스트 픽스처

```
tests/fixtures/www/
├── index.html
├── file.css
├── file.js
├── image.png          (최소 PNG)
├── big.bin            (1MB)
├── empty/             (index 없음)
├── dir/
│   └── index.html
├── .env               (dotfile, 차단 대상)
├── link-inside -> index.html
└── link-outside -> /etc/passwd
```

- 통합 테스트는 이 docroot를 복사하거나 그대로 사용한다.
- `link-outside`는 docroot 밖 symlink 차단 테스트용이다. 테스트 환경에서만 만든다.

## 3. 테스트 매트릭스

### A. 빌드 / 설정 (CFG)

| ID | 요구사항 | 레벨 | 시나리오 | 기대 | 상태 |
|---|---|---|---|---|---|
| CFG-01 | FR-01/NFR-03 | 통합 | `./server 8080 ./www` | 서버 기동, `curl` 접속 | TODO |
| CFG-02 | NFR-03 | 단위/통합 | 인자 없음 | usage 출력, exit != 0 | TODO |
| CFG-03 | NFR-03 | 단위 | 포트 0/65536/abc | 검증 실패 | TODO |
| CFG-04 | NFR-03 | 단위 | 없는 docroot | 검증 실패 | TODO |
| CFG-05 | NFR-03 | 단위 | 파일을 docroot로 지정 | 검증 실패 | TODO |
| CFG-06 | NFR-03 | 단위 | 없는 cgi_dir 지정 | 검증 실패 | TODO |
| CFG-07 | D11 | 단위 | 기본값 확인 | 4KB/16KB/100/8MB/20s/10s/100 | TODO |
| CFG-08 | D11 | 단위 | CLI override | 설정값 반영 | PASS (timeout) |

### B. TCP / 시그널 (TCP)

| ID | 요구사항 | 레벨 | 시나리오 | 기대 | 상태 |
|---|---|---|---|---|---|
| TCP-01 | FR-01 | 통합 | `curl` 접속 | 연결 수락, 응답 | PASS |
| TCP-02 | NFR-05 | 통합 | 서버 재시작 | `SO_REUSEADDR`로 bind 성공 | TODO |
| TCP-03 | NFR-10 | 통합 | `SIGINT` | 새 accept 중단, 정상 종료 | PASS (shutdown-test) |
| TCP-04 | NFR-10 | 통합 | `SIGTERM` | 정상 종료 | PASS (shutdown-test) |
| TCP-05 | NFR-10 | 통합 | 응답 중 클라이언트 끊기 | 서버 생존, `SIGPIPE` 무시 | PASS |
| TCP-06 | FR-02 | 통합 | accept 중 시그널 | `EINTR` 후 `g_stop` 확인, 정상 종료 | PASS (shutdown-test) |
| TCP-07 | FR-02 | 통합 | 이미 사용 중인 포트 | 명확한 오류 | TODO |
| TCP-08 | FR-02 | 통합 | 클라이언트 중간 끊기 | 크래시 없음, fd 회수 | PASS |
| TCP-09 | NFR-10 | 통합 | keep-alive 중 `SIGINT` | 현재 응답 후 종료, 다음 요청 거부 | PASS (shutdown-test) |
| TCP-10 | NFR-10 | 통합 | 두 번째 `SIGINT` | 즉시 종료 | PASS (shutdown-test) |
| TCP-11 | NFR-10 | 통합 | 응답을 읽지 않는 클라이언트 | `SO_SNDTIMEO` 후 close, 서버 생존 | PASS |

### C. HTTP 요청 파싱 (REQ)

| ID | 요구사항 | 레벨 | 시나리오 | 기대 | 상태 |
|---|---|---|---|---|---|
| REQ-01 | FR-03 | 단위 | 정상 GET 요청 라인 | method/URI/version 파싱 | PASS |
| REQ-02 | FR-05 | 단위 | 정상 POST + CL | 본문 길이/내용 | PASS (CGI POST) |
| REQ-03 | FR-04 | 단위 | HTTP/1.1 `Host` 없음 | 400 | PASS |
| REQ-04 | FR-04 | 단위 | HTTP/1.1 `Host` 있음 | 통과 | PASS |
| REQ-05 | FR-06 | 단위 | `FOO / HTTP/1.1` | 501/405 | PASS |
| REQ-06 | FR-03 | 단위 | `HTTP/9.9` | 505 | PASS |
| REQ-07 | FR-03 | 단위 | 잘못된 요청 라인 | 400 | PASS |
| REQ-08 | FR-04 | 단위 | `host`/`HOST` | 대소문자 무시 | PASS |
| REQ-09 | FR-04 | 단위 | 헤더 앞뒤 공백 | trim | PASS |
| REQ-10 | FR-04 | 단위 | 중복 CL, 같은 값 | 허용 | PASS |
| REQ-11 | FR-04 | 단위 | 중복 CL, 다른 값 | 400 | PASS |
| REQ-12 | FR-04 | 단위 | CL + TE | 400 | PASS |
| REQ-13 | FR-04 | 단위 | `Transfer-Encoding: chunked` | 501 + close | PASS |
| REQ-14 | D11 | 단위/통합 | URI 4KB 초과 | 414 | PASS |
| REQ-15 | D11 | 단위/통합 | 헤더 16KB 초과 | 431 | PASS |
| REQ-16 | D11 | 단위 | 헤더 100개 초과 | 431 | PASS |
| REQ-17 | D11 | 통합 | 본문 8MB 초과 | 413 | PASS |
| REQ-18 | FR-05 | 단위 | POST에 CL 없음 | 411/400 | PASS |
| REQ-19 | FR-03 | 단위 | `%20`, `%2e` | 디코딩 | PASS |
| REQ-20 | FR-03 | 단위 | `/path?q=1` | path/query 분리 | PASS |
| REQ-21 | FR-10 | 단위 | `..`, `%2e%2e` | 정규화/거부 | PASS (basic) |
| REQ-22 | D11 | 통합 | 느린 요청 | 408/close | PASS |
| REQ-23 | FR-03 | 통합 | `\r\n\r\n` 없이 끊김 | 400/timeout | PASS |
| REQ-24 | D10 | 통합 | `Expect: 100-continue` | 100 Continue 후 본문 | PASS |
| REQ-25 | D9 | 단위 | `Connection: close` | close 플래그 | PASS (통합) |
| REQ-26 | FR-03 | 단위 | HTTP/1.0 Host 없음 | 허용 | PASS |

### D. HTTP 응답 (RESP)

| ID | 요구사항 | 레벨 | 시나리오 | 기대 | 상태 |
|---|---|---|---|---|---|
| RESP-01 | FR-07 | 단위/통합 | 200 응답 | 상태 라인/헤더/본문 | PASS |
| RESP-02 | FR-08 | 단위/통합 | Content-Length | 실제 바이트 수와 일치 | PASS |
| RESP-03 | FR-08 | 단위/통합 | Content-Type | MIME과 일치 | PASS (html) |
| RESP-04 | D9 | 통합 | Phase 1 후반 기본 | `Connection: keep-alive` | PASS |
| RESP-05 | FR-08 | 단위 | 에러 응답 | 상태/헤더/본문 형식 | PASS |
| RESP-06 | FR-08 | 통합 | 404 | HTML body + CL | PASS |
| RESP-07 | FR-08 | 통합 | 403 | HTML body + CL | PASS |
| RESP-08 | FR-08 | 통합 | 400 | HTML body + CL | PASS |
| RESP-09 | FR-06 | 통합 | 405 | `Allow: GET, HEAD` | PASS |
| RESP-10 | D11 | 통합 | 413/414/431 | 정확한 상태/CL | PASS |
| RESP-11 | FR-08 | 통합 | 500 | 내부 오류 응답 | PASS |
| RESP-12 | FR-06 | 통합 | 501 | 미지원 메서드/TE | PASS |
| RESP-13 | FR-03 | 통합 | 505 | 미지원 버전 | PASS |
| RESP-14 | D10 | 통합 | HEAD | 헤더만, CL 동일 | PASS |
| RESP-15 | FR-07 | 통합 | Date/Server | 선택 헤더 형식 | SKIP (미구현) |

### E. 정적 파일 (STATIC)

| ID | 요구사항 | 레벨 | 시나리오 | 기대 | 상태 |
|---|---|---|---|---|---|
| STATIC-01 | FR-09 | 통합 | `/index.html` | 200 + MIME + body | PASS |
| STATIC-02 | FR-09 | 통합 | `/` | `index.html` | PASS |
| STATIC-03 | FR-09 | 통합 | `/missing` | 404 | PASS |
| STATIC-04 | FR-09 | 통합 | 권한 없는 파일 | 403 | PASS |
| STATIC-05 | FR-10 | 통합 | `/../etc/passwd` | 403/404 | PASS |
| STATIC-06 | FR-10 | 통합 | `/%2e%2e/...` | 403/404 | PASS |
| STATIC-07 | D12 | 통합 | `/.env` | 404 | PASS |
| STATIC-08 | D12 | 통합 | `/dir` | 301 `/dir/` | PASS |
| STATIC-09 | D12 | 통합 | `/dir/` | 200 `dir/index.html` | PASS |
| STATIC-10 | D12 | 통합 | `/empty/` | 404 | PASS |
| STATIC-11 | D12 | 통합 | docroot 안 symlink | 200 | PASS |
| STATIC-12 | D12 | 통합 | docroot 밖 symlink | 403 | PASS |
| STATIC-13 | FR-09 | 단위/통합 | html/css/js/png/jpg/json/txt/unknown | 올바른 MIME | PASS (html) |
| STATIC-14 | FR-09 | 통합 | 1MB 파일 | 스트리밍, 메모리 안정 | PASS (2MB) |
| STATIC-15 | D10 | 통합 | HEAD | 본문 없음, CL 동일 | PASS |
| STATIC-16 | D10 | 통합 | If-Modified-Since | 304, 본문 없음 | PASS |
| STATIC-17 | D10 | 통합 | Range 0-499 | 206 + Content-Range | PASS |
| STATIC-18 | D10 | 통합 | 잘못된 Range | 416 | PASS |
| STATIC-19 | D10 | 통합 | Accept-Ranges | `bytes` | PASS |
| STATIC-20 | D12 | 통합 | 디렉토리 listing | 생성 안 함 | PASS |
| STATIC-21 | D10 | 통합 | `OPTIONS /index.html` | 200 + `Allow` | PASS |
| STATIC-22 | FR-09 | 통합 | 전송 중 파일 truncate | 조기 종료, 다음 요청 정상 | PASS |
| STATIC-23 | FR-09 | 단위 | `send_file` 조기 EOF | `-1` 반환 | PASS |
| STATIC-24 | FR-09 | 단위 | `send_file` 정상 크기 | `0`, 전체 전송 | PASS |

### F. CGI (CGI)

| ID | 요구사항 | 레벨 | 시나리오 | 기대 | 상태 |
|---|---|---|---|---|---|
| CGI-01 | FR-11 | 통합 | GET `/cgi-bin/echo?x=1` | query echo | PASS |
| CGI-02 | FR-11 | 통합 | POST body echo | body echo | PASS |
| CGI-03 | FR-12 | 통합 | env vars | METHOD/QUERY/LENGTH/TYPE/SCRIPT/PROTOCOL/REMOTE | PASS |
| CGI-04 | FR-12 | 통합 | 없는 CGI | 404/500 | PASS |
| CGI-05 | FR-12 | 통합 | CGI 비정상 종료 | 500 | PASS |
| CGI-06 | FR-12 | 통합 | CGI 타임아웃 | 500/504 | PASS (504) |
| CGI-07 | FR-12 | 통합 | 큰 POST body | 교착 없음 | TODO |
| CGI-08 | FR-12 | 통합 | cgi_dir 밖 실행 | 차단 | TODO |
| CGI-09 | FR-12 | 통합 | fd 상속 | listenfd 미상속 | TODO |
| CGI-10 | FR-12 | 통합 | CGI 응답 헤더 | Content-Type/CL 처리 | PASS (echo) |

### G. keep-alive (KEEP) — Phase 1 후반

| ID | 요구사항 | 레벨 | 시나리오 | 기대 | 상태 |
|---|---|---|---|---|---|
| KEEP-01 | D9 | 통합 | Phase 1 초반 (역사적) | 항상 close | SKIP (late enabled) |
| KEEP-02 | D9/FR-14 | 통합 | HTTP/1.1 같은 연결 2+ 요청 | 둘 다 응답 | PASS |
| KEEP-03 | D9 | 통합 | HTTP/1.0 기본 | close | PASS |
| KEEP-04 | D9 | 통합 | HTTP/1.0 `keep-alive` | 유지 | PASS |
| KEEP-05 | D9 | 통합 | `Connection: close` | 응답 후 종료 | PASS |
| KEEP-06 | D9 | 통합 | idle timeout 10s | 종료 | PASS (idle 1s test) |
| KEEP-07 | D9 | 통합 | max requests 100 | 종료 | PASS (max 3 test) |
| KEEP-08 | D9 | 통합 | 에러 응답 후 keep-alive | close, 다음 요청 미처리 | PASS |
| KEEP-09 | D9 | 통합 | 버퍼 잔여 바이트 | 다음 요청으로 이어짐 | PASS |
| KEEP-10 | D9 | 통합 | `ab -k` | 정상 처리 | PASS |

### H. 비기능 (NFR)

| ID | 요구사항 | 레벨 | 시나리오 | 기대 | 상태 |
|---|---|---|---|---|---|
| MEM-01 | NFR-09/14 | 비기능 | ASan/UBSan 전체 테스트 | 클린 | PASS |
| MEM-02 | NFR-09 | 비기능 | 반복 요청 후 fd 수 | 누수 없음 | PASS (4 -> 4) |
| MEM-03 | NFR-09 | 비기능 | 클라이언트 중간 끊기 | 크래시/누수 없음 | PASS |
| LOAD-01 | NFR-13 | 비기능 | `ab -n 1000 -c 1` | 완료, 결과 기록 | PASS (약 2,319 RPS) |
| LOAD-02 | NFR-13 | 비기능 | `ab -k -n 1000 -c 1` | 완료, 결과 기록 | PASS (약 2,636 RPS) |
| PORT-01 | NFR-04 | 비기능 | macOS `make`/`make test` | 통과 | PASS |
| PORT-02 | NFR-04 | 비기능 | Linux(Docker/CI) | 통과 | TODO (Docker 데몬) |
| DOC-01 | NFR-16 | 비기능 | README | 빌드/실행/한계 문서화 | TODO (사용자가 나중에 작성) |

### 3.9 Phase 1.5 하드닝

| ID | 영역 | 시나리오 | 기대 | 상태 |
|---|---|---|---|---|
| HARD-01 | strict parser | 중복 Host, `Host :`, CTL, `CL: +5` | 400 | PASS |
| HARD-02 | HEAD | 에러/CGI 응답 body | body 없음 | PASS |
| HARD-03 | deadline | trickle 헤더 (0.4초 간격) | 408 | PASS |
| HARD-04 | CGI supervision | SIGTERM 무시, partial+exit 1 | 504/500, 서버 생존 | PASS |
| HARD-05 | CGI env | `PATH`, `HOME` | clean env (`PATH`만) | PASS |
| HARD-06 | access log | 요청 완료 | `ip "METHOD path" status bytes` | PASS |
| HARD-07 | lingering close | 4xx/5xx 후 잔여 body | 64KB/1초 drain | PASS (구현) |
| HARD-08 | 405/414/docroot | 405 Allow, 20KB URI, docroot dot, cgi_dir | 405/414/200/시작 거부 | PASS |
| HARD-09 | split points | 유효 요청의 모든 prefix | `HTTP_PARSE_INCOMPLETE` | PASS |
| HARD-10 | CGI contract | 중복 CT, 204 body, invalid Status | 500; 201 Created | PASS |
| HARD-11 | CGI no-body | 204/304 no body | CL/CT 없음 | PASS |
| HARD-12 | HEAD early error | 414/431 before parse | body 없음 | PASS |
| HARD-13 | CGI 205 | Reset Content | body 없음, CL 0 | PASS |
| HARD-14 | cgi_dir containment | docroot/cgi_dir 상호 포함 | 시작 거부 | PASS |
| HARD-15 | mixed URI overflow | 긴 URI + CRLF + 큰 헤더 | 414 | PASS |

## 4. 테스트 우선 구현 순서

1. **CFG/TCP**: 현재 스모크 테스트를 정리하고, 설정/시그널 테스트 추가.
2. **REQ**: 요청 라인/헤더/본문 파서 테스트를 먼저 작성 → 구현.
3. **RESP**: 응답 빌더/에러 응답 테스트 → 구현.
4. **STATIC**: D12 정책 포함 테스트 → 구현.
5. **CGI**: echo/time CGI 테스트 → 구현.
6. **KEEP**: Phase 1 후반에 테스트 → 구현.
7. **MEM/LOAD/PORT**: 마무리.

## 5. Definition of Done

- 테스트가 먼저 작성되어 **실패하는 것을 확인**했다.
- 구현 후 `make test` + `make integration`이 통과한다.
- 엣지/보안 테스트가 포함되어 있다.
- ASan/UBSan이 클린하다.
- 테스트 상태(`TODO`/`FAIL`/`PASS`)가 갱신되었다.
