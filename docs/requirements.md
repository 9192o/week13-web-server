# 요구사항 인덱스 — C 웹서버 (Week 13)

이 문서는 **인덱스**다. 실제 요구사항은 Phase별 파일로 나누어 관리한다.

## Phase별 명세

| Phase | 파일 | 범위 |
|---|---|---|
| Phase 1 | [phase1-requirements.md](phase1-requirements.md) | 동시성 이전: TCP/HTTP/정적/CGI/에러/keep-alive 후반 |
| Phase 2 | [phase2-requirements.md](phase2-requirements.md) | 동시성: fork/thread pool/event loop, keep-alive 유지/튜닝 |
| Phase 3 | [phase3-requirements.md](phase3-requirements.md) | 프록시: Reverse/Forward, 라우팅, 로드 밸런싱, 캐싱 |

Phase 1 테스트 계획: [phase1-test-plan.md](phase1-test-plan.md) — 테스트 케이스와 테스트 우선 순서.
Phase 1 품질 게이트: [quality-gate.md](quality-gate.md) — sanitize, fd, 부하, 이식성 결과.
아키텍처: [architecture.md](architecture.md) — 모듈, 요청 흐름, keep-alive/CGI 설계.
함수 흐름: [function_flow.md](function_flow.md) — 서버 구동/연결/keep-alive 흐름.
함수 레퍼런스: [function_reference.md](function_reference.md) — 함수별 역할, 파일:라인, 구현 의도.

## 공통 원칙

- 과제 명세를 **source of truth**로 삼는다.
- 명세에 없는 기능은 “설계 결정” 또는 “품질 기준”으로 표시하고, 구현자(사용자)가 결정한다.
- 의미 있는 결정은 **선택지 + 트레이드오프**를 제시한 뒤 사용자가 정한다. 사소한 기본값(에러 코드, 로그 형식, CLI 문법)은 문서에 표시하고 진행한다.
- 결정이 바뀌면 해당 Phase 파일과 이 인덱스의 결정 표를 함께 갱신한다.

### 구분 값

- **Must**: 과제 명세가 반드시 요구.
- **Should**: 권장. 시간/일정에 따라 조정 가능.
- **선택**: 명세가 선택적으로 허용.
- **설계 결정**: 명세가 구현자에게 맡긴 것.
- **품질 기준**: 과제의 품질/중점 포인트를 구체화한 것.

## 결정 현황

| ID | 결정 | Phase | 상태 |
|---|---|---|---|
| D1 | Phase 1 동적 처리 | 1 | **결정: CGI (C 프로그램)** |
| D2 | Phase 2 동시성 모델 | 2 | **미확정** (추천: pthread 스레드 풀) |
| D3 | Phase 3 프록시 종류 | 3 | **미확정** (추천: Reverse) |
| D4 | Reverse 라우팅 | 3 | **미확정** (추천: 경로 기반) |
| D5 | 프록시 정적 서빙 | 3 | **미확정** (추천: 직접 서빙) |
| D6 | 설정 방식 | 1/3 | **결정: CLI (Phase 1) + Phase 3 설정 파일** |
| D7 | 빌드 | 1 | **결정: Makefile** |
| D8 | 캐싱 | 3 | **미확정** (선택) |
| D9 | Phase 1 keep-alive 시점 | 1/2 | **결정: B+** |
| D10 | Phase 1 HTTP 기능 범위 | 1 | **결정:** HEAD, Last-Modified, OPTIONS, Range(단일). 100-continue 선택. ETag 보류 |
| D11 | Phase 1 요청 한계 | 1 | **결정: 커스텀 확정** (4KB/16KB/100/8MB/20s/10s/100) |
| D12 | 정적 파일 정책 | 1 | **결정: A안** (`/dir`→301, index 없음→404, symlink docroot만, dotfile 차단, `index.html`만) |
| D13 | 테스트 전략 | 1 | **결정: 커스텀 단위 하네스 + 셸 통합** |
| D14 | 이식성 목표 | 1 | **결정: macOS + Linux, POSIX 코어** |

## Phase별 핵심 결정 메모

### D1 — 동적 처리: CGI, C 프로그램 (확정)

- Phase 1은 CGI로 동적 요청을 처리한다.
- CGI 프로그램도 C로 작성한다.
- 서버는 `/cgi-bin/*` 요청을 `fork` + `execve`로 실행하고, 환경변수/stdin/stdout으로 요청/응답을 주고받는다.
- Phase 1을 자체 완결로 유지하고, Phase 3에서 프록시/백엔드 전달을 다룬다.
- **선택 이유:** 런타임 의존성이 없다(`make`로 서버+CGI 빌드), 서버/CGI의 에러 처리·메모리 모델이 일관된다, CGI 프로그램을 작게 유지할 수 있다.
- 상세: [phase1-requirements.md](phase1-requirements.md)

### D9 — keep-alive: B+ (확정)

- Phase 1 초반은 `Connection: close`만.
- 연결 루프/버퍼는 keep-alive 대비.
- 핵심 기능 안정 후 Phase 1 후반에 keep-alive 활성화. **구현/테스트 완료.**
- idle timeout 10초, max requests 100 (설정 가능).
- 상세: [phase1-requirements.md](phase1-requirements.md)

### D10 — HTTP 기능: 실용 + Range(단일) (확정)

- 포함: HEAD, Last-Modified/If-Modified-Since, OPTIONS, Range/206(단일 범위).
- 선택(비필수): Expect: 100-continue.
- 보류: ETag/If-None-Match (Last-Modified만 사용).
- Phase 3: chunked(프록시 필수), gzip(Phase 3에서 결정).
- 제외: Directory listing, Pipelining.
- 상세: [phase1-requirements.md](phase1-requirements.md)

### D11 — 요청 한계: 커스텀 (확정)

- URI 4KB, 헤더 16KB, 헤더 100개, 본문 8MB, 요청 타임아웃 20초.
- keep-alive idle 10초, max requests 100.
- CLI로 조정 가능. 상세: [phase1-requirements.md](phase1-requirements.md)

### D6 — 설정: CLI (Phase 1) + Phase 3 설정 파일 (확정)

- Phase 1은 CLI만: `./server <port> <docroot> [cgi_dir]`.
- Phase 3에서 라우팅 규칙/백엔드/타임아웃용 설정 파일을 추가한다.
- 이유: Phase 1은 설정이 적고, Phase 3는 라우팅 규칙이 많아 파일이 적합하다.
- 상세: [phase3-requirements.md](phase3-requirements.md)

### D7 — 빌드: Makefile (확정)

- 파일 수가 적고 외부 의존성이 없어 Makefile로 충분하다.
- CMake는 IDE/의존성 관리에 좋지만 이 프로젝트에는 과하다.
- 상세: [phase1-requirements.md](phase1-requirements.md)

## 공통 품질 기준

- **메모리**: `-fsanitize=address,undefined`; valgrind는 Linux/Docker에서 선택.
- **기능 테스트**: `curl`, 브라우저, `nc`로 정상/비정상 요청.
- **부하 테스트**: `ab`, `wrk`, `hey`.
- **엣지 케이스**: 잘못된 요청, 큰 본문, 긴 URI, 끊김, 백엔드 타임아웃.
- **문서**: README(빌드/실행/한계), 테스트 결과, 설계 결정 이유.

## Phase 간 의존성

- Phase 1 → Phase 2: `accept` 루프, 연결별 상태, 파서/응답/정적/CGI 모듈, keep-alive, 시그널 뼈대.
- Phase 2 → Phase 3: 동시성 모델, 타임아웃/backpressure, graceful shutdown.
- Phase 3: Phase 1의 HTTP 파서/응답 빌더와 Phase 2의 동시성을 재사용.

## 범위 제외 (전체)

- TLS, HTTP/2, HTTP/3
- 전체 RFC 7230~7235 준수 (최소 범위만)
- HTTP pipelining
- Directory listing
- gzip은 Phase 3에서 결정
