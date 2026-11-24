# week13-web-server

크래프톤 정글 13주차 학습 프로젝트. 별도 서버 프레임워크나 이벤트 루프 라이브러리 없이
C17과 Linux 소켓 API로 HTTP/1.1 서버를 구현한다.

현재 서버는 단일 스레드의 LT `epoll`로 여러 연결을 처리한다.
정적 파일 GET/HEAD, 바이너리 `POST /echo`, 연결 재사용, 순차 파이프라이닝,
SIGINT/SIGTERM 정상 종료를 지원한다. 과제 필수인 Forward/Reverse Proxy 중
하나 이상은 아직 구현하지 않았으며 Phase 3 범위다.

## 실행

기준 환경은 Linux/amd64 Docker다. 호스트에 Docker CLI와 Linux 컨테이너 데몬,
POSIX 셸이 필요하다. WSL에서는 Windows 드라이브의 저장소에서 `docker.exe`를 사용한다.

```sh
sh dev.sh build
sh dev.sh shell
```

개발 셸에서 다음을 실행한다. `--port`와 `--root`는 필수다.
아래 `--root public`은 공개용 샘플만 제공한다. 저장소 전체를 정적 루트로 쓰지 않는다.

```sh
make
./webserver --port 8080 --root public
```

기본 개발 컨테이너는 호스트에 포트를 공개하지 않는다. 같은 컨테이너에서 확인한다.

```sh
curl -i http://127.0.0.1:8080/
printf 'hello' | curl -i --data-binary @- http://127.0.0.1:8080/echo
```

## 폴더 구조

| 경로 | 책임 |
| --- | --- |
| `src/app/` | CLI 진입점, 요청 라우팅, echo 응답 |
| `src/http/` | HTTP 요청 파싱, 본문 프레이밍, 공통 응답 헤더 |
| `src/server/` | 리스너, epoll, 연결 I/O, 세션 상태, 정상 종료 |
| `src/static/` | 정적 루트 내 파일 탐색·열기 및 정적 응답 |
| `tests/` | 단위·소켓 회귀 검사, Docker 검증 및 벤치마크 |
| [`examples/`](examples/README.md) | 서버와 별도로 빌드하는 LT/ET 학습 실습 |
| `docs/` | 요구사항·설계·검증 결과와 재현 방법 |
| [`artifacts/`](artifacts/README.md) | Phase별로 보관한 검증 증빙과 벤치마크 원본 JSON |

## 검증

개발 셸에서 실행한다.

```sh
make test
make test-shutdown test-epoll test-keepalive
```

도구 환경은 호스트에서 `sh dev.sh check`, 메모리 검사는 개발 셸에서
`make sanitize`와 `make valgrind`로 확인한다. Docker 재실행 절차와 검사별
범위는 [Phase 2 검증 기록](docs/PHASE_2_TESTING.md)에 있다.

## 문서

- [HTTP 서버 코드 읽기](public/http-server-guide.html): 파일을 바로 열거나 실습 서버의 `/http-server-guide.html`로 접근
- [브라우저 라우팅·MIME 실습](docs/ROUTING_DEMO.md)
- [전체 과제 요구사항](docs/REQUIREMENTS.md)
- Phase 1: [요구사항](docs/PHASE_1_REQUIREMENTS.md) · [설계](docs/PHASE_1_ARCHITECTURE.md) · [검증](docs/PHASE_1_TESTING.md)
- Phase 2: [요구사항](docs/PHASE_2_REQUIREMENTS.md) · [검증](docs/PHASE_2_TESTING.md)
- [과거 진행 원장과 구현 계획](docs/archive/README.md)
