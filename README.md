# week13-web-server

크래프톤 정글 13주차 학습 프로젝트다. 외부 HTTP 서버·프레임워크·이벤트 루프
라이브러리 없이 C와 OS 소켓 API로 HTTP 서버를 구현한다.

## 실행 환경

웹서버는 **Linux Docker 컨테이너 안에서 실행한다.**
빌드·기능·보안·자원 검사와 성능 측정도 해당 환경을 기준으로 한다.
호스트 OS와 컨테이너의 실행 환경은 구분하며, macOS 네이티브 지원은 필수 범위가 아니다.

개발환경은 Debian 13의 `trixie-20261104-slim` 이미지와 C17·GCC·Make·Python을 사용한다.
GDB·strace·curl·Valgrind와 sanitizer 실행 환경도 포함한다.
현재 구성은 Linux/amd64 개발용이며, 요청 라인·헤더·CL/chunked 본문을 파싱해 앱에 전달하는 예제가 있다.
본문 전에 버전·Host·URI 구문과 고정 Router를 검사한다.
`GET/HEAD /`·`/index.html`은 실행 디렉터리의 index 파일, `POST /echo`는 바이너리 echo를 제공한다.
일반 정적 루트·하위 디렉터리·CLI·시간 제한 등 전체 서버 기능은 아직 구현하지 않았다.
실제 Docker 접근 상태와 미실행 사유는 [진행 원장](AGENTS.md#8-증빙-원장)에 기록한다.

## 개발환경 사용

호스트에는 Docker CLI와 접근 가능한 Linux 컨테이너 데몬, POSIX 셸이 필요하다.
호스트의 컴파일러·Make·Python 설치는 필요 없다. 일반 사용자 계정에서 실행한다.
WSL에서는 Docker Desktop의 `docker.exe`를 사용하고 `wslpath`로 bind mount 경로를
변환한다. WSL의 소스는 Windows 드라이브(`/mnt/c` 등)에 둔다.
일반 Linux 호스트에서는 `docker`를 사용한다.

```sh
sh dev.sh build
sh dev.sh check
sh dev.sh shell
```

- `build`: 개발 도구 이미지를 빌드한다. 소스는 이미지에 복사하지 않는다.
- `check`: 컨테이너에서 `make dev-check`를 실행한다. 작업 디렉터리 쓰기, C17·POSIX 컴파일/실행, ASan·UBSan·누수 검사 실행과 Valgrind를 확인한다.
- `shell`: `/workspace`에서 개발 셸을 연다. `exit`로 나가면 컨테이너를 제거한다.

소스는 읽기·쓰기 bind mount로 연결하고 호스트 UID/GID로 실행한다.
기본 이미지 사용자도 비특권 `dev`이며, 호스트가 root인 실행은 환경 검사에서 거부한다.
기본 포트 공개·privileged 모드·추가 capability·Docker 소켓 공유는 사용하지 않는다.
개발 컨테이너의 중지 유예는 35초다. 서버의 시그널 처리와 30초 정상 종료 유예는 Phase 2 시작에 구현한다.

`check`는 정상 C 프로그램을 실제 도구로 컴파일·실행하는 환경 확인이다.
HTTP 기능·보안·메모리 검사나 GDB/strace의 ptrace 동작 검증을 대신하지 않는다.
컨테이너에서 `make`를 실행하면 `webserver`를 빌드한다. 도움말은 `make help`다.
호스트에 Make가 있다면 `make dev-build`·`make dev-shell`도 같은 진입점을 호출한다.
`app.c`의 main에서 서버를 시작하고, `webserver.c`가 블로킹 방식으로 연결을 하나씩 처리한다.
한 번에 최대 1,024바이트씩 읽고 요청 라인·헤더·본문을 순서대로 파싱한다.
`http.c`에 요청 라인·헤더·본문 파싱 함수를 그 순서로 모으고 마지막에 공통 응답 생성기를 둔다.
본문 구역은 프레이밍·Expect·증분 디코딩을 담당한다.
헤더 뒤의 `http_validate_request_head()`는 HTTP/1.1 및 높은 1.x minor를 수용하고
1.0·다른 major는 505, 잘못된 Host·대상 구문은 400으로 본문/100 전에 거부한다.
origin·HTTP(S) absolute·CONNECT authority·OPTIONS asterisk를 구문 분리하고
원문 path/query/authority를 요청 구조체에 보존한다.
`router.c`는 path를 한 번 디코딩해 고정 경로만 비교하며 query를 무시한다.
알 수 없는 경로는 404, 알려진 경로의 메서드 불일치는 405와 Allow, 미구현 메서드는 501이다.
본문은 최대 1 MiB이며 NUL을 포함하는 바이너리 바이트를 보존한다.
앱은 index 파일을 본문 전에 열어 거부 가능 여부를 확인하고 echo 응답은 본문 완료 뒤 준비한다.
index는 고정 버퍼로, echo는 명시적 바이트 길이로 송신하며 HEAD는 본문을 보내지 않는다.
문법/미완성 EOF는 400, 본문 초과 413, 요청 라인 초과 414, 헤더/청크 메타데이터 초과 431,
미지원 Expect는 417, 미지원 전송 코딩은 501을 보내고 닫는다.
echo는 `application/octet-stream`·`nosniff`를 사용하고 요청 헤더를 응답에 복사하지 않는다.
성공·오류 최종 응답은 Date·Content-Type·Content-Length·Connection: close를 공통 생성한다.
파싱/프레이밍 오류도 고정 text/plain 사유를 보내며, 정상 요청 라인에서 HEAD를 판별한 뒤에는
오류에도 본문을 보내지 않는다. 시간 초과와 408 발생 조건은 아직 구현하지 않았다.
index의 최종 symlink·특수 파일은 403이며 임의 파일명·하위 경로는 열지 않는다.
컨테이너 안에서 다음과 같이 빌드·실행한다.

```sh
make
./webserver
```

레포 디렉터리에서 실행하고, 같은 컨테이너의 다른 터미널에서 확인한다.

```sh
curl -i http://127.0.0.1:8080/
curl -I http://127.0.0.1:8080/index.html
printf 'hello' | curl -i --data-binary @- http://127.0.0.1:8080/echo
```

Docker Desktop은 Linux 컨테이너 모드로 실행한다.
이 PC에서는 Ubuntu 내부 Docker 소켓 대신 Windows 클라이언트를 통해 연결한다.
기본 진입점의 이미지 빌드·환경 검사와 실제 PTY의 개발 셸 검증을 통과했다.
이미지 ID·도구 버전·검증 범위는 진행 원장에 기록했다.

## 구현 단계

| 단계 | 범위 | 상태 |
| --- | --- | --- |
| Phase 1 | 블로킹 순차 처리, HTTP/1.1, 정적 GET/HEAD, 바이너리 POST `/echo` | 파싱·head 검증·고정 index/echo Router 구현. 일반 정적 루트·CLI·마감 등 미구현 |
| Phase 2 전반 | 시작 시 정상 종료·시그널 처리, 여러 연결의 동시 처리 | 구현 전, 동시성 모델 미정 |
| Phase 2 후반 | keep-alive 연결 재사용 | 구현 전 |
| Phase 3 | Forward 또는 Reverse Proxy | 구현 전, 방식 미정 |

Phase 1의 주요 계약은 요청당 연결 종료, 본문 최대 1 MiB, 입력·시간 제한,
정적 루트 경계 검증과 필수 보안 테스트다. 세부 정책은 아래 명세를 따른다.
동시성·프록시를 포함한 전체 과제 완료와 Phase 1 완료를 구분한다.
2026-11-20 사용자 결정으로 정상 종료·시그널 처리는 Phase 1 완료 조건에서 제외하고
Phase 2 시작으로 옮겼다. 일반적인 연결·오류 경로의 자원 정리는 Phase 1에 유지한다.

## 문서

| 문서 | 내용 |
| --- | --- |
| [전체 요구사항](docs/REQUIREMENTS.md) | 전체 과제와 Phase별 결정 상태 |
| [Phase 1 요구사항](docs/PHASE_1_REQUIREMENTS.md) | 동작 계약과 확정·미확정 정책 |
| [Phase 1 아키텍처](docs/PHASE_1_ARCHITECTURE.md) | 책임·자원 수명·경계 설계와 구현 순서 |
| [Phase 1 테스트](docs/PHASE_1_TESTING.md) | P1/T/A 추적표, 결정적 검증과 증빙 기준 |
| [진행 원장](AGENTS.md) | D-01~D-08, 실제 실행 상태와 다음 작업 |

## 구현 시작점

개발환경 구성 파일과 도구 체인 검사를 작성했다.
실제 컨테이너 빌드·검사·셸 실행 증빙은 D-07의 개발환경 하위 결정에 기록했다.
Phase 1의 I/O는 블로킹으로 확정했다. 청취 주소·마감 적용·안전한 파일 열기 등의
남은 계약은 해당 기능 구현 전에 정하고, 서버 정상 종료 설계는 Phase 2 시작에 진행한다.
첫 구현 단위와 단계별 선행 결정은
[아키텍처 A-10](docs/PHASE_1_ARCHITECTURE.md#a-10-검증을-위한-경계와-구현-순서)을 따른다.
관련 계약이 DECIDED인 범위부터 구현하며, 아직 없는 구현의 실행 통과를
착수 조건으로 요구하지 않는다. 실행하지 않은 검사는 NOT_RUN으로 남긴다.
