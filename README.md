# week13-web-server

크래프톤 정글 13주차 학습 프로젝트. 별도 서버 프레임워크나 이벤트 루프 라이브러리 없이
C17과 Linux 소켓 API로 HTTP/1.1 서버를 구현한다.

현재 서버는 단일 스레드의 LT `epoll`로 여러 연결을 처리한다.
정적 파일 GET/HEAD, 바이너리 `POST /echo`, 연결 재사용, 순차 파이프라이닝,
SIGINT/SIGTERM 정상 종료를 지원한다. Phase 3의 `reverse-proxy`는 별도로
실행하며, 설정된 HTTP 백엔드로 요청을 전달한다.

## 실행

기준 환경은 Linux/amd64 Docker다. 호스트에 Docker CLI와 Linux 컨테이너 데몬,
POSIX 셸이 필요하다. WSL에서는 Windows 드라이브의 저장소에서 `docker.exe`를 사용한다.

```sh
sh dev.sh build
sh dev.sh shell
```

개발 셸에서 `make`로 두 실행 파일을 빌드하거나, `make webserver`와
`make reverse-proxy`로 각각 빌드한다. 두 대상은 공통 소스를 사용하지만 서로의
실행 파일을 필요로 하지 않는다.

### 웹서버 단독 실행

`webserver`는 프록시 없이 실행된다. `--port`와 `--root`는 필수다.
아래 `--root public`은 공개용 샘플만 제공한다. 저장소 전체를 정적 루트로 쓰지 않는다.

```sh
make webserver
./webserver --port 8080 --root public
```

두 실행 파일은 `127.0.0.1`에서 수신하며 개발 컨테이너는 호스트에 포트를 공개하지 않는다.
아래 요청은 서버와 같은 컨테이너에서 실행한다.

```sh
curl -i http://127.0.0.1:8080/
printf 'hello' | curl -i --data-binary @- http://127.0.0.1:8080/echo
```

### 리버스 프록시

프록시는 `make reverse-proxy`로 빌드한다. 이 프로젝트의 `webserver`가 아닌
별도의 HTTP 서버도 백엔드로 쓸 수 있다. 아래 예제는 `webserver`를 백엔드로 사용한다.
명령은 같은 개발 컨테이너의 서로 다른 셸에서 실행한다. `sh dev.sh shell`은 호출할
때마다 새 컨테이너를 시작하므로 추가 셸은 `docker exec -it <컨테이너-ID> bash`로
연다(WSL에서는 `docker.exe`). 앞의 웹서버가 8080을 사용 중이면 먼저 종료한다.

```sh
# 터미널 1: 백엔드
./webserver --port 8001 --root public
# 터미널 2: 프록시
./reverse-proxy --port 8080 --route /=127.0.0.1:8001
# 터미널 3: 클라이언트
curl -i http://127.0.0.1:8080/
printf 'hello' | curl -i --data-binary @- http://127.0.0.1:8080/echo
```

프록시는 `--root` 없이 시작하며, 백엔드가 내려가면 해당 경로에 `502`를 반환한다.
백엔드가 다시 올라오면 프록시를 재시작할 필요가 없다. 다른 HTTP 백엔드를
8002에 띄우고 `--route /api/admin/=127.0.0.1:8002`를 추가하면 더 긴
접두사가 우선한다. 경로와 쿼리는 보존되며 경로가 아무 규칙과도 일치하지
않으면 `404`다. 목적지는 클라이언트의 `Host`나 절대 URI로 바뀌지 않는다.
같은 접두사에 서로 다른 백엔드를 등록하면 설정 순서대로 라운드 로빈한다.
예를 들어 `--route /=127.0.0.1:8001 --route /=127.0.0.1:8002`는
8001 → 8002 → 8001 순으로 중계한다. 장애 시 다른 백엔드로 재시도하지
않는다. 백엔드 주소는 숫자 IPv4만 지원하며 DNS, TLS, 캐싱은 범위 밖이다.

## 폴더 구조

| 경로 | 책임 |
| --- | --- |
| `src/app/` | CLI 진입점, 요청 라우팅, echo 응답 |
| `src/http/` | HTTP 요청 파싱, 본문 프레이밍, 공통 응답 헤더 |
| `src/server/` | 리스너, epoll, 연결 I/O, 세션 상태, 정상 종료 |
| `src/proxy/` | 프록시 실행 진입점, 백엔드 라우팅, HTTP 중계 |
| `src/static/` | 정적 루트 내 파일 탐색·열기 및 정적 응답 |
| `tests/` | 단위·소켓 회귀 검사, Docker 검증 및 벤치마크 |
| [`examples/`](examples/README.md) | 서버와 별도로 빌드하는 LT/ET 학습 실습 |
| `docs/` | 요구사항·설계·검증 결과와 재현 방법 |
| [`artifacts/`](artifacts/README.md) | Phase별로 보관한 검증 증빙과 벤치마크 원본 JSON |

## 검증

호스트에서 현재 작업 트리의 Phase 1–3 기능·메모리 검사를 함께 실행한다.

```sh
sh dev.sh build
sh dev.sh check
sh tests/verify_phase3.sh
```

기능 검사만 실행하려면 개발 셸에서
`make -B test test-shutdown test-epoll test-keepalive test-proxy`를 사용한다.
기존 Docker 검증 결과와 재현 절차는 [Phase 1–3 통합 검증](docs/PHASE_3_TESTING.md)을 참고한다.

### WSL2 보조 검증

- **빌드·기능:** GCC 15로 두 대상의 독립 빌드와 실행을 확인했다. C 단위 테스트
  실행 파일 8개와 HTTP·정적 파일·타임아웃·정상 종료·epoll·연결 재사용·프록시
  통합 테스트가 통과했다. 백엔드 미기동 및 중단 시 `502`, 재기동 후 프록시
  재시작 없이 정상 응답을 확인했다.
- **뮤테이션:** Mull로 단위 테스트 5개를 대상으로 실행했다. Phase 1은
  `353 / 74 / 4`, Phase 2는 `86 / 88 / 6`, Phase 3은 `221 / 71 / 2`
  (검출 / 생존 / 시간 초과)였다. 전체 코드 커버리지나 과제 합격을 뜻하지 않는다.
- **Docker 재검증 필요:** 이 작업 트리에서 `sh tests/verify_phase3.sh`는 WSL의
  `docker.exe: Exec format error`로 검사 시작 전에 차단됐다. 아래 성능 측정과
  WSL 기능 통과는 제출 환경의 Docker 기능·메모리 검사를 대체하지 않는다.

### 성능 참고값

WSL2 루프백, GCC `-O2`, 요청마다 새 연결, 동시 클라이언트 32개 조건에서
4회 반복한 처리량과 회차별 p95의 중앙값이다. 프로덕션 성능 기준이 아닌
같은 환경에서의 직접 요청과 프록시 경유 비교다.

| 요청 | 직접 req/s | 직접 p95 | 프록시 경유 req/s | 프록시 경유 p95 |
| --- | ---: | ---: | ---: | ---: |
| 정적 GET 1 KiB | 1,149 | 40.35 ms | 1,104 | 42.30 ms |
| `POST /echo` 64 KiB | 1,128 | 39.51 ms | 870 | 55.33 ms |

동시 클라이언트 1·8·32개를 포함한 비교 전체에서 32,800건이 성공했고,
오류는 0건이었다. 측정 종료 후 두 프로세스의 파일 디스크립터 수는 시작 전과 같았다.

## 문서

- [HTTP 서버 코드 읽기](public/http-server-guide.html): 파일을 바로 열거나 실습 서버의 `/http-server-guide.html`로 접근
- [브라우저 라우팅·MIME 실습](docs/ROUTING_DEMO.md)
- [전체 과제 요구사항](docs/REQUIREMENTS.md)
- Phase 1: [요구사항](docs/PHASE_1_REQUIREMENTS.md) · [설계](docs/PHASE_1_ARCHITECTURE.md) · [검증](docs/PHASE_1_TESTING.md)
- Phase 2: [요구사항](docs/PHASE_2_REQUIREMENTS.md) · [검증](docs/PHASE_2_TESTING.md)
- Phase 3: [설계·구현 계약](docs/PHASE_3_ARCHITECTURE.md) · [Phase 1–3 통합 검증](docs/PHASE_3_TESTING.md) · [착수 초안](docs/PHASE_3_KICKOFF.md)
- [과거 진행 원장과 구현 계획](docs/archive/README.md)
