# 프로젝트 지침과 Phase 1 진행 원장

마지막 갱신: 2026-11-21

## 1. 목적과 현재 작업 범위

이 레포는 외부 HTTP/이벤트 루프 라이브러리 없이 C와 OS 소켓 API로
고동시성 HTTP 서버를 만드는 학습 프로젝트다.

- Phase 1: 블로킹 순차 HTTP 처리·정적 파일·바이너리 echo·필수 안전성 검증.
- Phase 2: 시작 시 정상 종료·시그널 처리, 다중 연결 동시 처리, 후반에 keep-alive.
- Phase 3: Forward 또는 Reverse Proxy.

웹서버의 실행 환경은 Linux Docker 컨테이너로 확정했다.
기존 Linux/WSL2·macOS 네이티브 양쪽 지원·검증 조건은 이 결정으로 대체한다.
호스트 OS와 서버 실행 환경을 구분하며 macOS 네이티브 구현·검증은 필수 범위가 아니다.
개발환경의 이미지·도구·마운트·비특권 실행 구성은 D-07에 기록했다.
개발 이미지·도구 체인·빌드/검사/셸 진입점의 실행 검증을 완료했다.
최소 커널·서버 포트와 전체 HTTP 서버 실행 증빙은 아직 미완료다.
서버 정상 종료 세부는 Phase 2 시작에 결정·검증한다.

Docker 개발환경과 도구 체인 검사 구성을 완료하고 실제 실행 결과를 기록했다.
Dockerfile·dev.sh·Makefile·환경 검사와 `webserver.c`의 기본 블로킹 연결 반복 예제가 있다.
`app.c`는 main 진입점·고정 index 파일/바이너리 echo/Router 오류 응답 준비를,
`app.h`는 서버가 호출할 앱 인터페이스를 담당한다. `router.c`·`router.h`가 경로와 메서드를 선택한다.
`webserver.h`의 `webserver_run()`을 앱이 호출하며 서버가 소켓 수명을 관리한다.
소켓 생성·수신·송신·연결 닫기는 `webserver.c`가 담당하며 HTTP 응답 헤더와 상태별 고정 오류 표현 생성은 `http.c`·`http.h`에 둔다.
요청 라인·헤더를 최대 32,768바이트까지 누적 수신하고 `http.c`의 본문 상태 머신으로
CL/chunked 본문을 디코딩하는 학습 예제다. 디코딩 본문은 최대 1 MiB다.
헤더 완료 뒤 `http_validate_request_head()`로 버전·대상 구문·Host를 검증한 뒤 본문을 처리한다.
head/프레이밍 검증 뒤 고정 Router를 선택하고 index/오류는 본문 전에,
echo는 본문 완료 뒤 앱에서 준비한다. 원문 path/query/authority를 요청 구조체에 보존한다.
`GET/HEAD /`·`/index.html`은 실행 디렉터리의 고정 index 파일, `POST /echo`는 바이너리를 그대로 반환한다.
query는 두 경로 모두 무시하며 다른 경로 404·메서드 불일치 405/Allow·미구현 메서드 501이다.
문법·미완성 EOF 400, 본문 초과 413, 요청 라인 초과 414, 헤더/메타데이터 초과 431,
Expect 미지원 417·전송 코딩 미지원 501을 송신한다. 추가 본문을 기다리는 100-continue도 처리한다.
본문 파서의 새 바이트 입력 계약은 요청 라인·헤더의 누적 입력 계약과 다르다.
`tests/test_http.c`·`tests/test_body.c`의 단위 테스트와 서버 단위/소켓 하네스를 사용한다.
HTTP/1.0·다른 major는 505, 잘못된 Host·대상 구문은 400으로 본문 추가 수신·100 전에 거부한다.
Router는 경로를 한 번 디코딩해 리터럴 비교만 한다. 일반 정적 루트/하위 디렉터리·
경로 정규화·내부 링크 허용·CLI·마감·전체 서버 테스트·벤치마크는 아직 구현하지 않았다.
2026-11-20 사용자 결정으로 커스텀 SIGINT/SIGTERM 종료 처리와 논블로킹 대기를 제거했다.
정상 종료·시그널 처리(P1-27/T-27)는 Phase 2 시작에 구현하며 Phase 1 완료 조건에서 제외한다.
일반적인 연결·오류 경로의 자원 정리, 송신 실패의 SIGPIPE 안전성 요구는 Phase 1에 유지한다.
문서 작성 요청을 코드 구현·의존성 설치·커밋·외부 배포 요청으로 확대하지 않는다.
이후 사용자의 명시적 구현 요청은 이 현재 작업 범위보다 우선한다.

## 2. 재개할 때 읽을 순서

1. 이 파일의 진행 요약·미해결 결정·마지막 검증·다음 작업.
2. `docs/REQUIREMENTS.md`: 전체 과제와 필수/조건부/권장 구분.
3. `docs/PHASE_1_REQUIREMENTS.md`: P1-01~P1-32와 확정/표준 보완/검토안.
4. `docs/PHASE_1_ARCHITECTURE.md`: A-01~A-10과 소유권·실패 경로.
5. `docs/PHASE_1_TESTING.md`: T-01~T-32, 추적표·증빙·결정적 동기화.
6. 실제 구현이 생기면 관련 코드·테스트·결과 파일도 읽는다.

사용자의 최신 결정이 최우선이다.
전체 과제 요구를 Phase 1에서 삭제하지 말고 해당 Phase로 명시적으로 연결한다.
Phase 1의 구체적인 확정 정책은 이 세 문서를 함께 갱신한다.
검토안을 사용자 합의나 표준 요구사항으로 오인하지 않는다.
충돌이 생기면 문서 하나만 조용히 바꾸지 말고 결정 원장에 근거와 영향을 기록한다.

## 3. 변경 원칙

- 요구사항 P1 ID는 유지한다. 새 요구는 새 ID를 추가하고 추적표와 테스트를 잇는다.
- 요구 하나가 바뀌면 아키텍처·예상 테스트 결과·진행 상태를 함께 검토한다.
- 코드가 없는데 있는 것처럼 파일·함수·타깃·검증 결과를 보고하지 않는다.
- 필수 보안 테스트는 생략·삭제·약화하거나 재시도로 통과시키지 않는다.
- 사용자 변경·다른 작업자의 변경은 되돌리지 않는다.
- 미리 불필요한 추상화·다중 동시성 모델·다른 Phase 기능을 만들지 않는다.
- 플랫폼별 분기는 허용하지만 공개 정책·경계·안전성 기준은 같아야 한다.
- 한 컨테이너 환경의 통과를 미검증 이미지·커널·마운트 조건이나 호스트 네이티브 실행의 통과로 복사하지 않는다.
- 실행하지 않은 환경·명령·검사기는 NOT_RUN 또는 BLOCKED로 남긴다.
- 진단·테스트 결과에도 인증정보·본문·실제 개인 경로를 불필요하게 저장하지 않는다.
- 커밋·배포는 별도의 사용자 요청 없이 수행하지 않는다.

## 4. 현재 진행 요약

| 영역 | 상태 | 근거 |
| --- | --- | --- |
| 인터뷰 합의 정리 | 작성됨 | PHASE_1_REQUIREMENTS의 확정 항목 |
| 요구사항 명세 | 작성됨, 정합성 검사·독립 검토 반영 | P1-01~P1-32 |
| 아키텍처 | 초안, 구현 증명 미완료 | A-01~A-10, 특히 D-02·D-05 |
| 테스트 설계 | 요청 라인·헤더·본문 및 수신 연결 테스트 추가 | T-04·05·07~11·24·26의 하위 범위, 전체 완료 아님 |
| 진행 원장 | 이 파일에 저장 | 결정·상태·증빙·다음 작업 |
| 개발환경 구성 | PASS | 기본 dev.sh의 build/check, 실제 PTY의 shell, 도구 체인 검사·정리 확인 |
| 서버 구현·빌드 | 세 파서·head 검증·고정 index/echo Router PARTIAL | 본문/파일 바이트 송신·HEAD·404/405/501 구현. 일반 root·CLI·마감 없음. 최신 실행은 아래 증빙 |
| HTTP 기능·보안·메모리 검사 | 세 파서 단위·수신 연결·실제 소켓·메모리 하위 검사 | 아래 본문·수신 연결 증빙. 전체 HTTP·보안·메모리 검사 통과가 아님 |
| Linux Docker 컨테이너 검증 | 환경·요청 수신/파싱 예제 PASS, 전체 HTTP NOT_RUN | 아래 단계별 증빙. 전체 기능·보안 검증과 구분 |
| Docker 데몬 접근 | 개발 진입점 PASS | WSL에서 Windows 클라이언트·서버 29.9.2 사용. Ubuntu 내부 Docker 소켓은 사용하지 않음 |
| 구현 착수 순서 | 준비 문서 작성 | A-10의 단계별 착수 조건·첫 구현 단위 |
| 성능 기준선 | NOT_RUN | 측정 자료 없음 |

문서가 존재하는 것과 Phase 1 완료는 다르다.
초안의 미해결 사항은 아래에서 확인하고 해당 작업 전에 닫는다.

## 5. 결정 원장

OPEN은 관련 구현이나 테스트 판정 전에 정책·알고리즘을 결정해야 한다는 뜻이다.
문서 작성의 실패나 이미 합의한 정책의 취소를 뜻하지 않는다.
일반 구현 세부는 담당자가 근거를 남겨 정하되,
지원 범위·외부 동작·사용자 의도가 바뀌면 사용자와 확인한다.

결정과 실행 증명을 분리한다.
OPEN에서 계약·알고리즘·예상 결과가 정해지면 DECIDED로 기록해 관련 구현을 진행할 수 있다.
실제 실행 증빙이 필요한 항목은 그 뒤 VERIFIED로 전환한다.
설계 결정을 위해 아직 없는 구현의 통과를 요구하거나,
실행을 위해 VERIFIED를 먼저 요구하는 순환 조건을 만들지 않는다.

| ID | 상태 | 미해결 내용 | 닫는 데 필요한 근거 |
| --- | --- | --- | --- |
| D-01 | OPEN, 고정 Router의 query 무시 DECIDED | 일반 정적 파일의 MIME 목록/대소문자/별칭, 3xx 코드·쿼리·Location, 인코딩된 구분자·dot segment·UTF-8 | 명시적 정책표, 인터뷰 제안과 확정 구분, T-17·19~21 예상값 |
| D-02 | OPEN, 보안 차단 | Linux 컨테이너의 링크/경계/TOCTOU 안전한 열기, 지원 커널·마운트, 내부 절대 링크, 숨김 링크, 순환, 디렉터리 교체·파일 변경 | DECIDED: 지원 Linux API 근거·알고리즘 불변 조건·위협 모델. VERIFIED: 구현 후 제어된 T-16·T-18·T-22 결과 |
| D-03 | DECIDED, 메타데이터/트레일러 단위 검증 VERIFIED | 메타데이터 초과 431·금지 trailer 400·그 외/중복 무시 | 아래 본문·수신 계약과 P1-08·11, 실제 단위·메모리 증빙 |
| D-04 | OPEN, head/프레이밍·고정 Router 하위 계약 DECIDED | 일반 정적 파일/추가 API의 자원 오류·본문·Content-Encoding·Expect 조합 | 아래 하위 결정과 일반 서버의 최종 판정표 구분 |
| D-05 | OPEN, 연결 수명 차단 | 100·최종·408 송신·연결 닫기 예산, 무진행 카운터·동시 만료, unread input·half-close와 응답 보존 | T-12·14·24·26의 타임라인·최장 점유 상한. T-27의 서버 종료 마감은 Phase 2 시작 |
| D-06 | OPEN | JSONL 필드명·단위·null/부분 송신·완전 송신 여부, 출력 채널과 로그 backpressure | 기계 판독 스키마와 오류 정책, 민감정보/로그 주입/출력 실패 테스트 |
| D-07 | OPEN, 실행 환경·기본 CLI·블로킹 순차 I/O DECIDED, 개발 도구 VERIFIED | 최소 커널·서버 포트 공개, 청취 주소/IP·동적 포트·중복 인자·SO_REUSEADDR. 시그널 전달·중지 유예·종료 대기는 Phase 2 시작 | T-02·T-03·T-04와 개발 도구 증빙. T-27은 Phase 2 시작에 별도 검증 |
| D-08 | OPEN | 테스트 시계·READY/barrier, 컨테이너의 메모리/FD 도구, 성능 기준선의 호스트·이미지·마운트·네트워크·자원 제한·도구·프로파일·반복/통계 | 실행 가능한 검증 절차, 도구 지원 확인, 실제 결과 저장. T-27 종료 단계 동기화는 Phase 2 시작 |

결정 행을 닫을 때 날짜·근거·결정 내용·영향받는 P1/T ID를 아래 변경 기록에 남긴다.
해결되지 않은 보안 경계를 호환성 fallback으로 약화하지 않는다.
사용자에게 이미 물은 내용은 대화·명세에서 먼저 확인하고 다시 묻지 않는다.

### 하위 결정의 저장 형식

한 D 그룹의 일부를 정했다고 전체를 닫지 않는다.
하위 항목별로 다음 정보를 이 원장 또는 해당 명세 절에 남긴다.

| 필드 | 필요한 내용 |
| --- | --- |
| 하위 항목 | 예: D-02의 내부 절대 링크, D-05의 408 송신 예산 |
| 출처·결정 | 사용자 확정 / 표준 보완 / 구현자 결정과 근거 |
| 계약 | 입력·순서·정확한 상태/헤더·마감·자원 정리 |
| 영향 | 관련 P1·A·T ID와 문서 위치 |
| 설계 상태 | OPEN 또는 DECIDED, 다음 담당 작업 |
| 검증 상태 | NOT_RUN / PASS / FAIL / BLOCKED, 실제 명령·플랫폼·결과 경로 |
| 닫는 조건 | 필요한 테스트·플랫폼·불변 조건과 남은 제약 |

특히 D-04는 요구사항의 「조합별 판정표」,
D-02는 아키텍처 A-06의 Linux 컨테이너 경계 증명 자료,
D-05는 100/408/최종 송신/닫기를 포함한 타임라인으로 구체화한다.
D-06의 JSON 키와 바이트 수 의미는 인터뷰에서 확정한 로그 항목 자체와 구분한다.

### D-07 하위 결정: 실행 환경

- 출처·결정: 2026-11-20 사용자 확정. 웹서버는 Linux Docker 컨테이너 안에서 작동한다.
- 계약: 빌드·실행·기능·보안·자원 검사와 성능 기준선은 해당 환경에서 수행한다. 기존 두 OS 네이티브 검증 조건을 대체하며 macOS 네이티브 지원은 필수 범위에서 제외한다.
- 영향: ENV-05, P1-02·12·18·27·29~31, A-01·06·08·10, T-02·18·27·29~31과 관련 명세.
- 설계 상태: 실행 환경 DECIDED. 이미지·최소 커널·CPU 아키텍처·도구·마운트·포트 세부는 D-02·D-07·D-08에서 계속 결정한다. 서버 정상 종료 세부는 Phase 2 시작에 다룬다.
- 검증 상태: 개발 도구·세 진입점 PASS, 전체 서버 환경은 PARTIAL. HTTP 서버·테스트는 아직 없다. 아래 개발환경 최종 실행 증빙을 따른다.
- 닫는 조건: 지원 컨테이너 환경을 구체화하고 실제 빌드·기능·보안·자원·성능 결과를 남긴다. D-02·D-07·D-08 전체를 닫은 것으로 간주하지 않는다.

### D-07 하위 결정: 기본 빌드·CLI

- 출처·결정: 현재 P1-03에 기록된 확정 계약을 원장과 T-03에 동기화한다. 이번 정리에서 새 옵션을 추가하지 않는다.
- 계약: Makefile을 제공한다. 실행 파일 이름은 `webserver`, `--port`/`--root`는 필수이며 포트는 1~65535다. 잘못된 인자·루트·바인드 실패는 진단 후 비정상 종료한다. 필수 두 인자의 생략 기본값은 없다.
- 영향: P1-03, A-02·A-10, T-03. 청취 주소·IPv4/IPv6·테스트용 동적 포트·중복 인자 세부는 OPEN으로 남긴다.
- 설계 상태: 위 기본 계약만 DECIDED. 검증 상태: Makefile 기본 빌드 PASS, CLI 인자 처리·검증은 미구현/NOT_RUN.
- 닫는 조건: 관련 세부 결정을 정한 뒤 빌드·CLI 성공/실패·FD 정리 결과를 남긴다.

### D-07 하위 결정: 개발 도구와 실행 진입점

- 출처·결정: 2026-11-20 사용자의 개발환경 구성 요청에 따른 구현자 결정. 서버 동작 정책을 새로 확정하지 않는다.
- 이미지·언어: `debian:trixie-20261104-slim`, Linux/amd64, C17. 개발 도구는 GCC·libc 개발 헤더·Make·Python 3·curl·GDB·strace·Valgrind와 CA 인증서다.
- 버전 근거: 날짜가 있는 공식 Debian 이미지 태그를 사용한다. 개별 패키지는 Debian 저장소에서 설치하고 실제 버전·이미지 ID를 빌드/검사 결과에 남긴다. 패키지까지 비트 단위로 고정한 재현 빌드를 보장하지 않는다.
- 실행 계약: `sh dev.sh build/check/shell`. 호스트에는 Docker CLI·데몬·POSIX 셸만 필요하다. Make·컴파일러·Python은 컨테이너 안에서 사용한다.
- 클라이언트 경계: WSL에서는 `docker.exe`와 `wslpath -w`로 변환한 Windows 드라이브의 소스 경로를 사용한다. 일반 Linux 호스트는 `docker`와 원래 경로를 사용한다. Ubuntu 내부 소켓·호스트 Docker 설정을 변경하지 않는다.
- 권한·마운트: 이미지 기본 사용자는 dev(1000:1000), 진입점은 호스트 UID/GID와 HOME=/tmp로 실행한다. 소스를 `/workspace`에 읽기·쓰기 bind mount로 연결하고 이미지에는 복사하지 않는다.
- 격리·종료: 포트·Docker 소켓을 공개하지 않고 추가 capability나 privileged 모드를 쓰지 않는다. `--init`·자동 컨테이너 제거·개발 셸 중지 유예 35초를 적용한다. 서버의 30초 정상 종료 계약은 Phase 2 시작에 별도 검증한다.
- 검사: `make dev-check`는 C17·POSIX 컴파일/실행, ASan/UBSan/누수 검사 설정 실행, Valgrind와 비특권 작업 디렉터리 쓰기를 확인한다. 전체 120초와 강제 종료 유예 5초이며 HTTP 테스트가 아니다.
- 영향: P1-02·P1-03·P1-29·P1-31, A-10, T-02·T-03·T-29·T-31의 개발환경 하위 범위.
- 상태: 개발환경 하위 결정 VERIFIED. WSL/Windows 드라이브에서 기본 build/check·실제 PTY shell·임시 자원 정리 PASS. 다른 호스트·마운트의 통과를 의미하지 않는다.
- 남은 범위: 최소 커널·파일 시스템 경계·서버 청취·검사 도구의 서버 적용·성능 조건은 D-02·D-05·D-07·D-08에서 계속 결정한다. 서버 정상 종료는 Phase 2 시작으로 분리한다.

### D-07 하위 결정: 첫 구현의 I/O와 종료 처리 시점

- 출처·결정: 2026-11-20 사용자 확정. Phase 1은 블로킹 순차 처리로 진행하고, 정상 종료·시그널 처리는 Phase 2 시작으로 옮긴다.
- 계약: `socket`·`bind`·`listen` 후 블로킹 `accept`를 반복한다. 첫 예제의 단일 recv는 요청 라인 서버 연결 단계에서 한 번에 최대 1,024바이트씩 누적하는 방식으로 확장했다. 요청 전체의 경계 판정은 아직 없다.
- 제거 범위: SIGINT/SIGTERM 핸들러·종료 플래그·시그널 마스크·pselect·O_NONBLOCK. 소켓 생성 실패와 일반 연결 처리의 close·오류 반환은 유지한다.
- Phase 구분: P1-27/T-27과 30초 정상 종료 유예는 Phase 2 시작에 구현·검증하며 Phase 1 완료 게이트에서 제외한다. SIGPIPE 송신 실패 안전성은 별도인 P1-26/T-26의 Phase 1 요구다.
- 영향: P1-04·P1-12·P1-27·P1-29·P1-32, A-01·A-02·A-04·A-08·A-10, T-27·T-29·T-32, D-05·D-07·D-08.
- 설계 상태: 위 범위 DECIDED. 블로킹 리스너·반복 연결 예제 PASS. 전체 HTTP·마감 적용·Phase 2 종료 검증은 NOT_RUN이다.
- 남은 범위: 블로킹 I/O에 수신·송신 마감을 적용할 세부 방식과 SO_REUSEADDR는 D-05·D-07에서 정한다. 이 결정으로 기존 HTTP 시간 제한을 삭제하지 않는다.

### D-04 하위 결정: 요청 라인 구문 분리

- 출처·결정: 2026-11-20 사용자 구현 요청에 따른 구현자 결정. RFC 9112 §2.3·§3의 버전 표기·요청 라인 구문과 기존 엄격한 파싱·8,192바이트 계약을 사용한다.
- 계약: 누적 버퍼에서 메서드 token·원문 요청 대상·한 자리 major/minor를 분리한다. 구분자는 한 SP, 종료는 CRLF다. 요청 대상의 URI 유효성·지원 버전·지원 메서드 판정은 별개이며 이 함수가 승인하지 않는다.
- 소유권·오류: 결과 필드는 원본 버퍼를 참조한다. COMPLETE는 요청 라인만의 완료이며 소비 길이는 CRLF 포함이다. NEED_MORE/ERROR는 소비 0·빈 필드, 문법 오류 400·길이 한도 위반 414를 반환한다. 실제 응답 송신은 아직 연결하지 않았다.
- 영향: P1-05·P1-10, A-03, T-05·T-10의 요청 라인 하위 범위.
- 상태: 구문 분리 구현·단위 검증 VERIFIED. 서버 지원 버전·request-target 형태·URI 정책과 D-04 전체는 OPEN이다. 헤더·본문·소켓 통합은 후속 작업이다.

### D-04 하위 결정: 독립 헤더 구문 파서

- 출처·결정: 2026-11-20 사용자 헤더 함수 구현 요청에 따른 구현자 결정. RFC 9112 §2·§5와 P1-05·P1-10의 엄격한 구문·기존 한도를 적용한다.
- 계약: `http_parse_headers()`는 요청 라인 직후 누적 버퍼와 정상 요청 라인의 소비 길이를 받는다. 헤더 이름은 token, 값은 앞뒤 SP/HTAB을 제외한 원문 참조다. 이름 대소문자·중복·순서를 보존하며 이름 비교는 후속 의미 검증에서 대소문자를 구분하지 않는다.
- 경계·오류: 빈 줄까지의 소비 길이는 헤더 시작 기준이다. 본문은 검사·소비하지 않는다. NEED_MORE/ERROR는 소비 0·빈 목록이다. 잘못된 구문·금지 제어문자·obs-fold는 400, 필드 줄 CRLF 포함 8,192바이트·요청 라인/종료 빈 줄 포함 32,768바이트·100개 제한은 431이다.
- 영향: P1-05·P1-10, A-03, T-05·T-10의 구문/제한 하위 범위. 중복 필드를 보존하는 것은 Host/CL 등의 중복 승인이 아니며 P1-05·P1-07의 의미 검증 요구를 유지한다.
- 상태: 구문 단위·메모리 검증 VERIFIED. 이후 본문·수신 연결에서 서버 연결·CL/TE·Expect 해석을 추가했다. Host·지원 버전·URI 검증과 D-04 전체는 OPEN이다. 단계별 실행 증빙은 아래를 따른다.

### D-04 하위 결정: Router 전 요청 head 의미 검증

- 출처: 2026-11-20 사용자 요청으로 Router 선행 문서를 점검하고 검증을 구현한다.
  사용자가 HTTP/1.1 기반 유지를 선택했다. HTTP/1.0·다른 major는 505,
  높은 1.x minor는 1.1 규칙으로 처리한다. 지원하지 않는 버전 표기와 구문 오류를 구분한다.
- 표준 근거: RFC 9110 §2.5·§7.2, RFC 9112 §3.2~3.3, RFC 3986 §2~3.
  origin/absolute/CONNECT authority/OPTIONS asterisk 문법, Host 단일 필드·authority 우선,
  URI 구성요소 문자를 적용한다. 공식 RFC 본문을 실제 조회했다.
- 구현자 결정: HTTP(S) absolute URI만 수용한다. 기본 authority를 설정하지 않으므로
  origin/asterisk의 빈 Host는 400이다. 자체 authority가 있는 absolute/CONNECT에서는
  빈 Host를 허용하되 Host 누락·중복·비어 있지 않은 잘못된 값은 모두 400이다.
  port는 URI 구문만 검사하며 endpoint 범위나 DNS를 검증하지 않는다.
  정확한 표와 빈 port·IPvFuture·zone ID 범위는 P1-05 하위 계약에 기록했다.
- 순서·소유권: 헤더 구문/한도 → 버전 → 대상 구문 → Host → CL/TE·Expect → 본문.
  의미 오류에서는 본문 추가 recv·할당·100이 없다. 이미 한 recv로 들어온 바이트와 구분한다.
  `http_target`의 form/authority/path/query는 원문 참조이고 빈 absolute path만 정적 `/`다.
  path/query는 아직 percent-encoded이며 실패 출력은 빈 구조체다.
- 영향: P1-05·P1-19·P1-20·P1-24, A-03·A-05, T-05·T-24. 세 명세와 README 동기화.
- 설계 상태: 위 하위 계약 DECIDED. 검증 상태는 아래 최신 head 검증 증빙을 따른다.
  고정 Router의 query·본문·coding·메서드/자원 오류와 Expect 순서는 다음 하위 결정에서
  추가로 정했다. 일반 파일의 encoded separator·dot segment·UTF-8 정책은 계속 OPEN이다.
  이 항목이 D-01·D-04 전체나 Router 구현 완료를 뜻하지 않는다.

### D-01·D-02·D-04 하위 결정: 고정 index/echo Router

- 출처: 2026-11-20 사용자 구현 요청 및 「고정 두 경로」 선택. 실행 디렉터리의
  `index.html`만 `/`·`/index.html`로 제공하고 두 라우트에서 query를 무시한다.
  일반 정적 루트·하위 디렉터리·CLI root는 이번 요청에 포함하지 않는다.
- 계약: 한 번 percent decode 후 리터럴 비교. dot segment·중복/끝 슬래시를 정규화하지
  않고 일치하지 않으면 404다. GET/HEAD index, POST echo만 구현한다.
  미구현 메서드 501 → 등록 경로 존재 판정 404 → 해당 경로의 허용 메서드 405/Allow 순서다.
  index 파일 열기는 그 뒤이므로 POST index는 실제 파일 유무와 관계없이 405다.
- 파일 경계: 사용자 경로를 open 인자로 만들지 않는다. 신뢰한 cwd의 고정 리터럴을
  O_NOFOLLOW·O_NONBLOCK·O_CLOEXEC로 열어 fstat 일반 파일만 제공한다.
  최종 링크/특수 파일 403, 없음 404, 기타 내부 실패 500이며 실패한 FD는 닫는다.
  이 제한된 경로는 일반 내부 링크 허용 알고리즘의 fallback이 아니며 D-02 전체는 OPEN이다.
- 순서: head 의미 검증 → CL/TE·Expect 초기화 → Router/index 열기 → 필요한 본문 →
  최종 응답. 헤더만으로 판정한 Router/파일 오류는 100·추가 본문 없이 거부한다.
  GET/HEAD의 유효한 프레이밍 본문은 공통 한도로 수신 후 버린다.
  Content-Type/Content-Encoding은 해석하지 않고 echo 바이트를 그대로 돌려주며 헤더에는 복사하지 않는다.
- 소유권: echo 응답은 디코딩 본문을 송신 종료까지 빌린다. index 응답 FD는 서버 소유이며
  fstat 크기 스냅샷까지만 고정 버퍼로 읽고 모든 성공/실패에서 닫는다.
  HEAD는 표현 길이를 보존하되 본문을 송신하지 않는다.
- 영향: P1-06·13~20·23~25, A-03·05~07, T-06·13~20·23~26의 고정 라우트 하위 범위.
  세 명세와 README를 함께 갱신했다. 설계 DECIDED, 검증은 아래 최신 증빙을 따른다.
- 남은 범위: 일반 root·링크/숨김/정규화·디렉터리·파일 공개 정책·CLI·마감·접근 로그.
  기존 파싱/프레이밍 오류의 빈 응답도 유지한다. 고정 Router 완료와 Phase 1 완료는 다르다.

### 변경 기록

2026-11-21 HTTP 응답 생성 통합 (사용자 구현 요청에 따른 구현자 결정):
기존 파싱/프레이밍의 빈 오류 응답을 앱 오류와 같은 `http_response` 생성·송신 경로로 통합한다.
오류 본문은 상태별 영어 사유 구문(끝 개행 없음), `text/plain; charset=utf-8`이며,
Date·표현의 Content-Length·Connection: close·nosniff를 공통 생성한다.
405의 Allow와 별도 100 Continue는 유지한다. 정상 요청 라인 구문 완료 직후 HEAD를 기억해
이후 헤더·head 의미·프레이밍·본문 오류에서도 본문을 억제한다.
요청 라인 자체의 실패에서 메서드를 추정하지 않는다.
408 표현은 생성하되 실제 시간 초과·송신/닫기 예산은 D-05의 후속 범위다.
영향: P1-13·23·25, A-07, T-13·23·25. 실행 결과는 아래 HTTP 응답 통합 증빙에 기록한다.

2026-11-20 본문·수신 연결 구현 계약 (구현자 결정, 설계 DECIDED, 하위 범위 검증 VERIFIED):
`http.c`의 본문 구역은 CL/TE 프레이밍·Expect 해석과 새 바이트 기반 본문 상태 머신을 담당한다.
서버는 요청 라인·헤더 누적 버퍼를 유지하고 본문은 증분 디코딩하며,
완료 후 `http_request`를 앱에 한 번 전달한다. 앱의 정상 응답은 빈 200으로 유지한다.
청크 줄/확장 누적/trailer 줄·총량·개수 초과는 431, 디코딩 본문 초과는 413,
문법·숫자 오버플로·프레이밍/금지 trailer 오류는 400으로 구분한다.
trailer의 Host·Content-Length·Transfer-Encoding·Connection·Trailer·TE·Upgrade·Expect·
Content-Encoding·Content-Type·Authorization·Proxy-Authorization·Cookie는 거부한다.
그 외 유효한 trailer는 중복을 포함해 무시하고 초기 헤더·앱 본문에 합치지 않는다.
여러 Expect 필드/목록의 단순 100-continue 반복은 허용하고,
알 수 있는 문법의 미지원 기대 조건은 417, 문법 오류는 400이다.
이미 완성된 본문·빈 본문에는 100을 생략하며 추가 수신이 필요하면 한 번 보낸다.
메서드와 무관하게 유효한 프레이밍 본문을 읽는 과도기 예제이며 라우팅·지원 버전·
URI/Host 검증·Content-Encoding 정책·마감/종료 예산은 이 작업으로 확정하지 않는다.
영향: P1-04·05·07~11·24·26, A-02~04, T-04·05·07~11·24·26.
아래 본문·수신 연결 검증에서 실제 명령·결과와 전체 Phase 1 미완료 범위를 구분한다.

2026-11-20 요청 라인 서버 연결 하위 계약: 사용자 연결 요청에 따른 구현자 결정.
정상 요청 라인은 기존 앱의 빈 200, 문법 오류는 400, 길이 제한은 414로 연결한다.
추가 입력이 필요하면 누적 수신하고, 완성 전 EOF는 빈 입력도 400이다.
요청 전체·지원 버전·메서드·URI·라우팅 판정은 아직 구현하지 않으며 D-04 전체를 닫지 않는다.
영향은 P1-04·05·10·13·26, A-02·03, T-04·05·10·13·26의 하위 범위다.
설계 DECIDED, 요청 라인 서버 연결 하위 범위 VERIFIED. 아래 서버 연결 증빙을 따른다.

| 날짜 | 변경 | 영향 |
| --- | --- | --- |
| 2026-11-19 | Phase 1 인터뷰를 세 명세와 진행 원장으로 옮김 | P1-01~32, T-01~32 |
| 2026-11-19 | 기본 문서 없는 디렉터리는 이전 403 제안을 대체해 404로 기록 | P1-17·22 |
| 2026-11-19 | 루트 이탈·권한 거부는 403 유지, 숨김·없음은 404 | P1-18·22·23 |
| 2026-11-19 | 부가 정보 수치는 표준값이 아닌 자체 제한, MIME 등 미확정 세부는 OPEN | P1-11·21, D-01·03 |
| 2026-11-19 | 독립 검토 반영: 조합별 오류 표, 큰 파일 송신 중단, 링크 형태별 지원 결정과 증명 자료, QA 단계별 추적 보완 | P1-07·16·18·25·31, T-12·18·28, D-01~08 |
| 2026-11-20 | 사용자 확정: Linux Docker 컨테이너 실행·검증으로 통일. 기존 Linux/WSL2·macOS 네이티브 양쪽 지원 조건을 대체하고 세부 환경·경계·종료·측정 계약은 OPEN 유지 | ENV-05, P1-02·12·18·27·29~31, T-02·18·27·29~31, D-02·07·08 |
| 2026-11-20 | README·전체 과제의 Phase별 결정 상태·문서 경로를 동기화하고 요구사항 제목의 P1 ID를 복원. 기본 CLI는 재질문 없이 확정 계약으로 연결하고, P1-11에서 빠진 최대값 열은 기존 합의·T-11의 8,192/32,768/32,768/8,192바이트·100개로 복원 | P1-01~32, T-03·T-11·T-21·T-32, D-03·D-07 |
| 2026-11-20 | A-10에 컨테이너·빌드부터의 단계별 착수 조건을 정리. Docker 접근 실패는 BLOCKED로 기록하고 관련 서버·테스트 상태는 NOT_STARTED/NOT_RUN 유지 | P1-02·P1-03·P1-29~P1-32, T-02·T-03·T-32, D-07·D-08 |
| 2026-11-20 | Docker 개발 이미지·dev.sh·Makefile·도구 체인 검사 구성. WSL의 클라이언트·경로 경계를 Windows Docker로 맞춰 기본 build/check/shell을 실제 검증하고 개발 도구 하위 결정을 VERIFIED로 기록 | D-07의 개발 도구, P1-02·P1-03·P1-29·P1-31의 환경 하위 범위만 해당 |
| 2026-11-20 | 사용자 확정: 첫 구현은 블로킹 순차 서버로 진행. 커스텀 종료 코드·pselect·O_NONBLOCK을 제거하고 정상 종료·시그널 처리와 30초 종료 유예를 Phase 2 시작으로 이관. 기존 P1/T ID와 전체 과제 요구는 유지 | P1-04·P1-12·P1-27·P1-29·P1-32, A-01·A-02·A-04·A-08·A-10, T-27·T-29·T-32, D-05·D-07·D-08 |

## 6. 요구사항별 실행 상태

상태는 구현/테스트 실행/플랫폼 증빙을 분리한다.
초기값의 `-`는 PASS가 아니라 구현과 실행이 없음을 뜻한다.
아래 표는 서버 요구사항 전체 기준이다. 개발환경의 하위 단위는 위 D-07과 증빙 원장에서 따로 기록한다.

| 요구사항 | 구현 | 테스트 실행 | Linux Docker 증빙 | 호스트·이미지 정보 |
| --- | --- | --- | --- | --- |
| P1-01 | NOT_STARTED | NOT_RUN | - | - |
| P1-02 | NOT_STARTED | NOT_RUN | - | - |
| P1-03 | PARTIAL: 기본 make 빌드, CLI 미구현 | PARTIAL: 빌드 PASS, CLI NOT_RUN | 기본 빌드 PASS | 아래 make 빌드 증빙 |
| P1-04 | PARTIAL: 블로킹 accept·세 파서 수신 연결·close | PARTIAL: 요청 수신/파싱 하위 검사 PASS | 아래 본문·수신 연결 증빙 | 개발 컨테이너·격리 검사 컨테이너 |
| P1-05 | PARTIAL: 요청 라인·헤더·본문 연결, Host/버전/URI 미검증 | PARTIAL: 구문·프레이밍 단위/소켓 PASS | 아래 본문·수신 연결 증빙 | 개발 컨테이너·격리 검사 컨테이너 |
| P1-06 | NOT_STARTED | NOT_RUN | - | - |
| P1-07 | CL/TE 해석·중복/충돌/코딩 검사 구현 | 단위·오류 응답 소켓 PASS | 아래 본문·수신 연결 증빙 | 개발 컨테이너·격리 검사 컨테이너 |
| P1-08 | 청크 디코더·확장/trailer 구문/정책 구현 | 단위·청크 소켓 PASS, 전체 T-29 아님 | 아래 본문·수신 연결 증빙 | 개발 컨테이너·격리 검사 컨테이너 |
| P1-09 | PARTIAL: 1 MiB·조기 거부·점진 할당, 최종 라우팅 우선순위 미정 | CL/chunked 경계 단위·소켓 PASS | 아래 본문·수신 연결 증빙 | 개발 컨테이너·격리 검사 컨테이너 |
| P1-10 | 요청 라인·헤더의 줄/총량/개수 제한과 서버 연결 | 단위 경계·400/414/431 소켓 PASS | 아래 본문·수신 연결 증빙 | 개발 컨테이너·격리 검사 컨테이너 |
| P1-11 | 청크 줄·확장 누적·trailer 한도/431 구현 | 메타데이터 단위·메모리 PASS | 아래 본문·수신 연결 증빙 | 개발 컨테이너 |
| P1-12 | NOT_STARTED | NOT_RUN | - | - |
| P1-13 | 현재 200/4xx/5xx 공통 헤더·고정 오류 표현·100·부분 송신 구현 | 단위·실제 소켓 PASS. 3xx 자원 기능은 후속 범위 | HTTP 응답 통합 증빙 | 아래 2026-11-21 증빙 |
| P1-14 | NOT_STARTED | NOT_RUN | - | - |
| P1-15 | NOT_STARTED | NOT_RUN | - | - |
| P1-16 | NOT_STARTED | NOT_RUN | - | - |
| P1-17 | NOT_STARTED | NOT_RUN | - | - |
| P1-18 | NOT_STARTED | NOT_RUN | - | - |
| P1-19 | NOT_STARTED | NOT_RUN | - | - |
| P1-20 | NOT_STARTED | NOT_RUN | - | - |
| P1-21 | NOT_STARTED | NOT_RUN | - | - |
| P1-22 | NOT_STARTED | NOT_RUN | - | - |
| P1-23 | PARTIAL: 상태별 고정 text/plain·공통 헤더 구현. 실제 408 발생/송신 제한은 P1-12 후속 | 파싱/Router 오류 소켓·통제된 open/fstat 500 C 단위 PASS | HTTP 응답 통합 증빙 | 아래 2026-11-21 증빙 |
| P1-24 | PARTIAL: 100-continue·417/400·빈/완료 본문 생략 | 기본 단위·실제 100 왕복 PASS, 버전/마감 미검증 | 아래 본문·수신 연결 증빙 | 개발 컨테이너·격리 검사 컨테이너 |
| P1-25 | 정상 요청 라인에서 HEAD 판별 후 성공·오류 본문 억제. 3xx 기능은 후속 | 단위·오류/미완성 EOF 실제 소켓 PASS | HTTP 응답 통합 증빙 | 아래 2026-11-21 증빙 |
| P1-26 | PARTIAL: send 반복·EINTR·MSG_NOSIGNAL·FD 정리 | PARTIAL: reset·부분 송신·EPIPE 경로 | 하위 예제 PASS, 전체 T-26 NOT_RUN | 아래 최소 200 응답 예제 증빙 |
| P1-27 | PHASE_2: 시작 시 구현 | NOT_RUN | Phase 1 게이트 제외 | - |
| P1-28 | NOT_STARTED | NOT_RUN | - | - |
| P1-29 | NOT_STARTED | NOT_RUN | - | - |
| P1-30 | NOT_STARTED | NOT_RUN | - | - |
| P1-31 | NOT_STARTED | NOT_RUN | - | - |
| P1-32 | NOT_STARTED | NOT_RUN | - | - |

추적 관계의 단일 표는 PHASE_1_TESTING §4다.
구현 시작 시 코드 위치와 실제 개별 테스트 이름을 함께 추가한다.
한 그룹 안의 필수 사례가 일부만 통과했다면 PARTIAL이며 PASS가 아니다.
실패·미실행 사유를 결과 경로 없이 완료로 대체하지 않는다.

## 7. 누락 재검증 절차

매 재개와 의미 있는 변경 후 다음을 수행한다.

1. 최신 사용자 결정과 원래 REQUIREMENTS를 읽고 Phase 누락·범위 확대를 확인한다.
2. 모든 P1 ID에 A 위치·T 그룹·예상 결과가 있는지 확인한다.
3. 각 테스트 그룹의 정상·오류·분할·경계·중단·정리 경로를 실제 코드와 대조한다.
4. D-01~D-08에서 아직 oracle·알고리즘·플랫폼 증명이 없는 항목을 찾는다.
5. 구현한 범위의 단위·통합·보안·메모리/FD 검사를 실행한다.
6. 실행 명령·exit code·환경·결과 경로를 증빙 원장에 기록한다.
7. 관련 요구의 상태와 다음 작업을 갱신한다. 완료된 조사를 불필요하게 반복하지 않는다.

문서 작성 단계에서는 ID·상호 참조·정책 충돌·표준/자체 정책 구분을 검토한다.
문서 검토를 실제 서버 기능·보안 검사의 통과로 표시하지 않는다.
Phase 1 범위의 필수 보안 실패·컨테이너 환경 증빙 누락·미해결 차단 항목이 있으면 Phase 1 완료가 아니다.
Phase 2 시작으로 이관한 P1-27/T-27의 미실행은 Phase 1 완료 차단 사유가 아니다.

## 8. 증빙 원장

### 이전 문서 검증

2026-11-19 문서 정합성 검사:

- P1 요구사항 32개, T 테스트 그룹 32개, A 설계 절 10개를 확인했다.
- 추적표 32행·진행표 32행·OPEN 결정 8그룹의 참조·중복을 검사했다.
- 정의되지 않은 ID, 누락 연결, 표 열 개수와 코드 블록 불일치가 없었다.
- 기존 README.md·REQUIREMENTS.md는 작업 시작 시 읽은 내용과 동일하다.
- 새 파일은 요청한 세 명세와 AGENTS.md뿐이다.
- C/Python·빌드·기능·보안·플랫폼 실행 검사는 실행 대상이 없어 NOT_RUN이다.

독립 누락 분석: analysis 역할, DeepSeek V4.1 Flash,
작업 ID `st_01a1f41a`, COMPLETED. 주 담당자가 대화·표준·문서와 대조해 다음처럼 반영했다.

- 오류 조합·우선순위의 확정/OPEN 표를 요구사항의 「조합별 판정표」에 추가했다.
- 큰 파일이 송신 마감에 걸리면 짧은 본문 뒤 종료하고 두 번째 500을 보내지 않는 테스트를 추가했다.
- 링크 형태별 지원을 이미 전부 확정한 것으로 보이지 않게 설계 검토로 분리했다.
  사용자가 최종 구성요소 링크만 허용했다고 결정한 것도 아니므로 그런 제한은 추가하지 않았다.
- 로그의 부분 송신·바이트 단위, 동적 포트/READY, 기준선 대상 OS를 결정 원장에 연결했다.
- 전체 QA를 Phase 1 완료로 묶지 않고 QA-01~QA-06별 후속 Phase를 명시했다.
- 공백·한글 이름, 특수 파일 403, 408/414/431은 대화에서 확인된 정책이므로 취소하지 않았다.
- 모든 버전 차이를 505로 처리하거나 모든 HEAD에 표준상 CL이 필수라는 제안은 채택하지 않았다.
  RFC 9110의 major/minor 구분과 HEAD 규칙을 재확인했다.
- 진단 로그에 필요한 모든 경로 정보를 일괄 금지하는 새 요구는 추가하지 않았다.
  합의대로 접근 로그의 실제 경로와 두 로그의 불필요한 민감정보 노출을 막는다.

문서 검토는 완료했지만 D-01~D-08의 세부 계약과 실제 실행 증명은 OPEN이다.
진행 상태를 구현·보안 테스트 통과로 해석하지 않는다.

### 2026-11-20 구현 준비 문서 검증 (개발환경 구성 전)

- 범위: README·전체 요구사항·Phase 1 요구사항/아키텍처/테스트·이 진행 원장, 총 6개 문서.
- 방법: 세션의 JS eval로 `read` 결과를 대조한 일회성 검사다. 회귀 테스트 파일이나 빌드 타깃을 추가하지 않았다.
- 결과: PASS. P1 정의 32개, T 정의 32개, A 정의 10개와 추적표·미실행 진행표 각 32행을 확인했다.
- 문서 링크·앵커 7개, ID 참조, 표 열 수·코드 블록 짝, 기존 요구사항 제목·순서와 복원한 청크 최대값을 확인했다.
- 기존 두 OS 네이티브 검증 의무는 컨테이너 기준으로 대체됐고, 확정된 기본 CLI와 후속 결정의 경계를 확인했다.
- 제약: 이 결과는 문서 정합성 검사다. Docker 접근은 아래 사전 점검에서 BLOCKED이며 서버·기능·보안·메모리·성능 검사는 NOT_RUN이다.

독립 구현 준비 점검: explore 역할, `opencode-go/deepseek-v4.1-flash`,
작업 ID `st_01a1f6b7`, COMPLETED. 현재 문서와 대조해 다음처럼 처리했다.

- D-02의 알고리즘 결정(DECIDED)과 구현 후 실행 증명(VERIFIED)을 분리했다.
- Docker 접근 차단 중에도 D-07의 계약 문서화는 진행할 수 있게 README·A-10·다음 작업을 맞췄다.
- API 이식성 설명 범위를 실제 지원 대상인 Linux 컨테이너의 POSIX·Linux 전용 API와 커널·마운트 제약으로 구체화했다.
- 검토 중 이미 갱신한 문서 검증 기록과 복원한 청크 한도를 오래된 상태로 되돌리지 않았다.

### 실행 결과

전체 요구사항 기준의 HTTP 서버·보안·메모리·성능 실행 결과는 없다.
기본 연결·수신·고정 200 응답 예제의 하위 범위 실행 결과는 아래에 별도로 기록한다.
개발환경 빌드·검사·셸 결과는 아래 최종 검증에 별도로 기록한다.
결과가 생기면 아래 형식의 실제 값으로 행을 추가한다.

```text
날짜 / 실행 ID:
플랫폼·도구 버전:
소스 상태:
관련 P1 / T / D:
실제 명령·작업 디렉터리:
exit code:
PASS / FAIL / NOT_RUN / BLOCKED:
결과 파일:
제약·다음 조치:
```

대용량 결과는 예를 들어 artifacts/phase1 아래에 두고 이 원장에는 요약·경로만 남긴다.
준비된 결과 디렉터리나 명령 예제가 있다는 것만으로 실행한 것으로 표시하지 않는다.

### Docker 접근 사전 점검

- 날짜 / 실행 ID: 2026-11-20 / monitor `mon_KED7T0NCCKN8KW2M`.
- 플랫폼·도구: 현재 WSL2 배포판, Docker CLI 29.9.2. 데몬·컨테이너·이미지 정보는 확인하지 못했다.
- 소스 상태: 문서만 있고 Dockerfile·Makefile·서버·테스트는 없다.
- 관련 항목: P1-02·P1-31, T-02·T-31, D-07·D-08.
- 작업 디렉터리: 레포 루트. 실제 명령은 다음과 같다.

```sh
if command -v docker >/dev/null 2>&1; then docker version --format 'client={{.Client.Version}} server={{if .Server}}{{.Server.Version}}{{else}}unavailable{{end}}'; else printf 'DOCKER_CLI_MISSING\n'; fi
```

- exit code: 1.
- 결과: BLOCKED. `client=29.9.2 server=unavailable`. 데몬 오류는 `The command 'docker' could not be found in this WSL 2 distro.`이며 WSL 통합 활성화를 안내했다.
- 근거: 위 monitor의 실행 출력. 별도 결과 파일은 생성하지 않았다.
- 제약·다음 조치: Docker Desktop의 해당 WSL 배포판 통합·데몬 접근을 확보한 뒤 같은 조회로 클라이언트와 서버 정보를 확인한다. 계정·호스트 설정 변경이나 설치는 수행하지 않았다.
- 빌드·서버·기능·보안·자원·성능 검사는 모두 NOT_RUN이다. 이 사전 점검을 컨테이너 실행 증빙으로 계산하지 않는다.

### 2026-11-20 개발환경 초기 검증 (WSL 진입점 전환 전)

- 관련 항목: D-07 개발 도구 하위 결정, P1-02·P1-03·P1-29·P1-31의 환경 범위만 해당한다.
- 소스 상태: Dockerfile·.dockerignore·dev.sh·Makefile·tests/environment를 작성했다. 사용자의 빈 webserver.c는 변경하지 않았다.
- 호스트 셸 검사: `sh -n dev.sh`와 `sh -n tests/environment/check.sh`, 도움말·잘못된 액션·추가 인자 종료 코드 검증 PASS. monitor `mon_HWPKQEBYR7521YC1`, 종료 코드 0. 경로 수정 후 구문 재검사도 `mon_RA0VAGMQWDA81W1A`, 종료 코드 0.
- 호스트 Make는 설치되어 있지 않다. 이 환경에서 `make dev-check`를 직접 실행한 초기 시도는 종료 코드 127이었다. 기본 진입점은 호스트 Make 없이 동작하는 `sh dev.sh`다.
- 첫 빌드는 Windows 연동 클라이언트가 Linux 절대 context 경로를 해석하지 못해 실패했다. dev.sh에서 레포로 이동한 뒤 상대 context `.`을 전달하도록 수정했다.
- 이미지 빌드: 레포 루트에서 `sh dev.sh build`, monitor `mon_GW4CD3R02T4RNK1Y`, 종료 코드 0, PASS.
- 이미지: `week13-web-server-dev:trixie`, ID `sha256:742b1e91b8289bfa0fd91555e2063b7cd2e8992daa5e266239a92ef598a82762`, linux/amd64, 기본 사용자 dev.
- 실행 경계: Windows docker.exe 클라이언트·서버는 29.9.2다. Ubuntu와 새 Ubuntu 세션 모두 `/usr/bin/docker`·`/var/run/docker.sock`이 없고 일반 `docker version`·개발 셸 진입은 WSL 통합 오류로 실패했다. Windows의 Docker 데몬 자체가 없는 상태와 구분한다.
- 실제 환경 검사: Windows 클라이언트에서 아래 명령으로 같은 소스를 Linux 컨테이너에 연결해 실행했다. 레포 루트가 작업 디렉터리다.

```sh
"/mnt/c/Program Files/Docker/Docker/resources/bin/docker.exe" run --rm --init --platform linux/amd64 --user "$(id -u):$(id -g)" --env HOME=/tmp --mount "type=bind,source=$(wslpath -w "$PWD"),target=/workspace" --workdir /workspace --stop-timeout 35 week13-web-server-dev:trixie make dev-check
```

- 결과: monitor `mon_TZTC8SBGFQ45541D`, 종료 코드 0, `DEV_ENV_OK`. Linux 6.18.33.2-microsoft-standard-WSL2, x86_64, UID/GID 1000:1000에서 작업 디렉터리 쓰기·C17/POSIX·ASan/UBSan/누수 검사 설정·Valgrind 실행 PASS.
- 도구: GCC 14.2.0, Make 4.4.1, Python 3.13.5, GDB 16.3, strace 6.13, Valgrind 3.24.0, curl 8.14.1, libc 개발 헤더 2.41. 세부 Debian 패키지 버전은 위 monitor 출력에 있다.
- 제약: 이 결과는 Linux 개발 이미지 자체의 검사다. Ubuntu의 `sh dev.sh check/shell` 성공이나 HTTP 서버의 기능·보안·메모리 검증으로 확대하지 않는다. GDB/strace의 ptrace 기능도 NOT_RUN이다.
- 당시 차단: 실제 사용 중인 배포판은 Ubuntu였다. 아래 최종 검증에서는 기본 진입점의 클라이언트·경로 경계를 수정해 해소했다.

### 2026-11-20 개발환경 최종 검증

- 변경: dev.sh는 WSL에서 Windows의 `docker.exe`를 직접 호출하고 bind mount 경로를 `wslpath -w`로 변환한다. 웹서버·검사 프로그램은 그대로 Linux 컨테이너에서 실행한다. 호스트 설정·Docker 소켓·권한은 변경하지 않았다.
- 진입점 검사: 레포 밖 `/tmp`에서 레포의 dev.sh를 실행했다. 셸 구문·잘못된 액션/추가 인자의 종료 코드 2, `build`와 `check` 모두 PASS. monitor `mon_2VZQAV5S75VKJVSB`, 전체 종료 코드 0.
- 이미지: `week13-web-server-dev:trixie`, ID `sha256:6fdaf83a899150c0e1c7dd4e843ea2e45e74d91779d22653f07db8640b8a54df`, linux/amd64, 기본 사용자 dev. 베이스 해석 digest는 `sha256:a72b4433de1cf7d6bae0c4cee74d0547f58601a643b3375f070c51600d63b6b2`다.
- 환경 검사: `DEV_ENV_OK`, 종료 코드 0. C17·POSIX·ASan/UBSan/누수 검사 설정·Valgrind 실행과 UID/GID 1000:1000의 작업 디렉터리 쓰기 PASS. 커널·패키지 버전은 위 초기 검증과 동일하며 최종 monitor에도 출력했다.
- 개발 셸: 세션의 Bun.Terminal로 실제 PTY를 열고 Bash 준비 프롬프트 이후에 검증 명령을 보냈다. `sh dev.sh shell`의 UID 1000, `/workspace`, stdin TTY, `make -n dev-build dev-shell`, `DEV_SHELL_OK`, 정상 종료 코드 0을 확인했다.
- 검사 채널 제약: 일반 bash/monitor 도구는 이 세션에서 pipe fallback이라 `-t` 실행이 실패했다. 실제 PTY 검사에서는 Windows 터미널의 커서 위치 질의에도 응답해 동일 진입점을 검증했다. 파이프 실행 실패를 제품의 PTY 실패나 성공으로 오인하지 않았다.
- 정리: 검사와 셸 종료 후 해당 이미지의 실행 중 컨테이너가 없고 `.dev-env-check.*` 임시 파일도 없음을 확인했다. 사용자 webserver.c는 수정하지 않았다.
- 범위: 개발환경만 PASS다. HTTP 서버·CLI·서버 보안·메모리/FD·성능은 NOT_RUN, GDB/strace ptrace 검증과 다른 호스트·마운트도 NOT_RUN이다.
- 언어 서버: 호스트의 clangd·bash-language-server·docker-langserver가 없어 LSP 검사는 실행하지 않았다. 호스트에 설치하지 않았으며 C 코드는 컨테이너 GCC의 `-Wall -Wextra -Wpedantic -Werror`, 셸은 `sh -n`과 실제 실행으로 검사했다.

### 2026-11-20 블로킹 순차 연결 예제 검증

- 소스: `webserver.c`. 종료·시그널·논블로킹 코드를 제거하고 기본 소켓 초기화·반복 accept·클라이언트 close를 유지했다.
- 환경: `week13-web-server-dev:trixie` Linux/amd64 이미지, 비특권 UID/GID 1000:1000. 별도 임시 컨테이너에 소스를 읽기 전용으로 마운트하고 `--network none`의 루프백에서 검사했다. 사용 중인 개발 컨테이너는 중지하지 않았다.
- 빌드 명령: 컨테이너의 `/workspace`에서 `gcc -std=c17 -Wall -Wextra -Wpedantic -Werror webserver.c -o <임시 디렉터리>/webserver`.
- 검사: Python 표준 라이브러리 하네스를 `docker.exe run --rm --init ... python3 -c`로 실행했다. 준비 출력 이벤트 뒤 `/proc/<pid>/fdinfo`로 O_NONBLOCK 미설정을 확인하고 20회 연결 수락·EOF와 FD 개수 유지를 검사했다. 고정 sleep·접속 재시도는 사용하지 않았다.
- 전환 전: monitor `mon_REAE4129M8H7RY3G`, exit 1. `Listener must be blocking`으로 기존 논블로킹 설정을 검출했다.
- 전환 후: monitor `mon_3V3TTYA4NK0PKKJT`, exit 0. `BUILD_PASS`, `BLOCKING_LISTENER_PASS`, `REPEATED_CONNECTIONS_PASS count=20`, `DEFAULT_SIGTERM_PASS exit=-15`, `BLOCKING_CHECK_PASS`.
- 종료 의미: SIGTERM의 기본 동작으로 종료된 결과다. 애플리케이션 정상 종료나 T-27 통과로 계산하지 않는다.
- 결과 근거: 위 monitor 출력. 별도 테스트·결과 파일은 생성하지 않았고 임시 실행 파일·컨테이너는 검사 종료 후 제거했다.
- 제약: HTTP 기능·마감·보안·메모리 검사와 Phase 2 정상 종료는 NOT_RUN이다. 호스트 clangd가 없어 LSP 대신 컨테이너 GCC의 엄격한 경고 검사와 실제 소켓 실행을 사용했다.
- 문서 교차 검토: explore 역할, `opencode-go/deepseek-v4.1-flash`, 작업 `st_01a1f7eb`, COMPLETED. 현재 파일과 대조해 종료 단계·완료 게이트의 남은 모호한 표현을 보완했다. 이미 수정된 P1-12 문장은 다시 변경하지 않았다.

### 2026-11-20 단일 데이터 수신 예제 검증

- 변경: `webserver.c`는 연결 후 즉시 닫지 않고 블로킹 `recv`로 최대 1,024바이트를 한 번 수신·출력한 뒤 닫는다. EINTR만 재호출하며 EOF·수신 실패도 클라이언트 소켓을 닫고 다음 연결을 받는다.
- 경계: 수신한 길이만큼 `fwrite`하며 NUL 종료 문자열로 가정하지 않는다. 한 번의 수신이 HTTP 요청 전체라는 가정은 하지 않는다. HTTP 파싱·응답·마감·정상 종료는 추가하지 않았다.
- 환경·명령: 앞선 검사와 같은 개발 이미지의 별도 비특권 임시 컨테이너·읽기 전용 소스 마운트·격리 루프백에서 `gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g webserver.c -o <임시 디렉터리>/webserver`와 Python 표준 라이브러리 하네스를 실행했다.
- 전환 전: monitor `mon_0JPJ9ZWXA6RM19PB`, exit 1. 즉시 close 뒤 데이터를 읽지 않아 클라이언트에서 ConnectionResetError가 발생했다.
- 전환 후: monitor `mon_G8KGDJA99NTBEPA8`, exit 0. `BUILD_PASS`, `BINARY_RECEIVE_PASS`, `EOF_RESET_RECOVERY_AND_REPEAT_PASS count=20 fds=4`, `RECEIVE_CHECK_PASS`. NUL·0xff 출력, 데이터 없는 EOF, 연결 reset 후 후속 연결 수신·FD 유지 확인.
- 제약: 기본 수신 예제 검사이며 HTTP·메모리 검사 통과로 계산하지 않는다. 테스트·결과 파일은 추가하지 않았고 임시 실행 파일·컨테이너는 정리했다.

### 2026-11-20 서버·앱 파일 분리 검증

- 변경: 수신 데이터 출력 처리를 `app.c`의 `app_handle_data(const char *, size_t)`로 옮기고 `app.h`에 선언했다. 소켓 수신·연결 관리는 `webserver.c`가 담당한다. 기존 app.c의 사용자 주석은 유지했다.
- 현재 컴파일: 컨테이너에서 `gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g webserver.c app.c -o <임시 디렉터리>/webserver`. 두 소스를 함께 빌드해야 한다.
- 검증: 분리 전 monitor `mon_E062XQ06CFCKZTX7`, 분리 후 monitor `mon_R6TV4W7HZJEA9RDZ` 모두 exit 0. 바이너리 수신 출력·빈 입력 EOF·reset 후 복구·20회 반복 연결·FD 개수 4 유지 확인.
- 범위: 기존 단일 수신 동작을 유지한 파일 분리다. HTTP 파일·200 응답·빌드 타깃은 추가하지 않았다.
- 후속 진입점 조정: main을 `app.c`로 옮기고 기존 서버 실행 본문을 `webserver_run()`으로 공개했다. 선언은 새 `webserver.h`에 둔다. 앱에서 서버를 시작하고 수신 시 앱 처리 함수를 호출하는 흐름이다.
- 진입점 조정 검증: monitor `mon_7S9DA46PHPEPR9PZ`, exit 0. 같은 두 소스의 엄격한 컴파일·바이너리 수신·EOF/reset 복구·20회 반복 연결·FD 유지 검사 PASS.

### 2026-11-20 임의 입력에 대한 최소 200 응답 검증

- 사용자 요청: 현재 학습 단계에서는 요청 문법·내용·바이트 값을 검사하지 않고 200 OK를 보낸다. 이 과도기 예제로 향후 P1-05~P1-12의 파싱·오류·입력 제한 계약을 대체하지 않는다.
- 책임: `http.c`의 `HTTP_OK_RESPONSE`는 `HTTP/1.1 200 OK`, `Content-Length: 0`, `Connection: close`와 CRLF 헤더 종료를 제공한다. `app_handle_data()`는 이 상수의 포인터를 반환하고 `webserver.c`가 전체 응답을 송신한다. 입력이 더 필요하다고 대기하는 HTTP 파서는 아직 없다.
- 송신: 부분 송신은 남은 바이트를 반복 전송하고 EINTR는 재호출한다. MSG_NOSIGNAL로 연결이 끊긴 클라이언트의 송신 실패가 프로세스를 SIGPIPE로 종료하지 않게 한다. SIGINT/SIGTERM 정상 종료 기능은 추가하지 않았다.
- 환경·빌드: 앞선 검사와 같은 개발 이미지의 비특권·격리 루프백 임시 컨테이너에서 `gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g webserver.c app.c http.c -o <임시 디렉터리>/webserver`와 Python 표준 라이브러리 하네스를 실행했다.
- 기준선: monitor `mon_ERTZVYXKY11ZZQZ0`, exit 1. 기존 서버가 응답 없이 닫아 HTTPResponse에서 RemoteDisconnected가 발생했다.
- 실제 소켓 검증: monitor `mon_GA4XC8TR4Z07EC53`, exit 0. 정상 HTTP 요청·비HTTP 텍스트·전체 바이트 값·NUL·빈 입력 후 송신 half-close·64 KiB 입력에서 200/CL=0/Connection: close를 확인했다. reset 뒤 20회 요청·FD 개수 4 유지와 실제 curl의 200도 PASS.
- 송신 경계 검증: monitor `mon_173GB4C6X3CJ4DKN`, exit 0. 테스트 번역 단위에서 실제 클라이언트 처리 함수를 호출하고 linker wrap으로 send/recv만 제어했다. 3바이트 단위 송신·EINTR 후 정확한 전체 응답, EPIPE 뒤 후속 처리, MSG_NOSIGNAL을 확인했다. Broken pipe 진단은 의도한 실패 주입 결과다.
- 제약: 고정 응답 학습 예제만 검증했다. HTTP 메시지 경계·크기 제한·Date·요청별 상태 판정·연결 정리 예산·정적 파일·echo·메모리 검사는 미구현/NOT_RUN이다. 테스트·결과 파일은 추가하지 않았고 임시 실행 파일·컨테이너는 정리했다.

### 2026-11-20 기본 make 빌드 검증

- 변경: Makefile 기본 목표를 `webserver`로 바꾸고 세 C 소스·세 헤더를 선행 조건으로 지정했다. C17·엄격한 경고 옵션으로 컴파일하며 기존 개발환경 타깃과 `make help`는 유지한다.
- 명령·결과: `docker.exe exec week13-web-server-dev make`, monitor `mon_DEEY2ATDPG24G65Y`, exit 0. `/workspace`에서 `gcc -std=c17 -Wall -Wextra -Wpedantic -Werror app.c webserver.c http.c -o webserver`를 실행해 소스 디렉터리에 실행 파일을 생성했다.
- 범위: 기본 빌드만 추가했다. CLI·HTTP 파싱·정상 종료 등 서버 동작은 변경하지 않았다.

### 2026-11-20 요청 라인 파서 검증

- 변경: `http.c`·`http.h`의 `http_parse_request_line()`과 `tests/test_http.c`, `make test-unit`. 서버의 기존 단일 수신·고정 200 응답 흐름은 아직 파서를 호출하지 않는다.
- 환경: 실행 중인 `week13-web-server-dev`의 `/workspace`, Linux/amd64 개발 이미지, 기존 비특권 사용자. LSP는 clangd가 없어 NOT_RUN이며 GCC의 `-Wall -Wextra -Wpedantic -Werror`로 검사했다.
- 미구현 확인: monitor `mon_8RE0SBC0BVAKKVGJ`, exit 1. 테스트를 먼저 작성한 뒤 엄격하게 컴파일하자 파서 함수·결과 타입 미선언으로 실패했다.
- 검사 명령: `docker.exe exec week13-web-server-dev`로 `make test-unit`, `make`, `make test-unit CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer"`를 실행했다. 세 단계 모두 성공했고 일반·sanitizer 테스트에 `HTTP_REQUEST_LINE_TESTS_PASS`가 출력됐다.
- 위 세 단계를 실행한 monitor `mon_NQDQWQQCKMCM4RZY`의 전체 exit는 1이다. 후속 Valgrind 준비 명령의 중첩 셸 인용이 잘못되어 `trap: rm: bad trap`으로 실패했다. 이를 파서 실패나 전체 명령 성공으로 기록하지 않는다.
- 인용 수정 후 Valgrind만 실행: monitor `mon_PBMXMTKD7A800S41`, exit 0. `gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g -I. tests/test_http.c http.c -o <임시 파일>` 뒤 `valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 <임시 파일>`. `HTTP_REQUEST_LINE_TESTS_PASS`, 종료 시 0바이트·0블록, 오류 0건이며 실행 파일은 trap으로 제거했다.
- 사례: 정상 필드·소비 길이·원본 참조, 모든 단일 분할점·한 바이트씩 누적, NUL/제어문자·잘못된 토큰/CRLF/버전·잘못된 공백, 요청 라인 8,191/8,192/8,193바이트와 미완성 한도, 후속 바이트 비소비.
- 범위·결과: 요청 라인 단위·ASan/UBSan·Valgrind PASS, 서버 빌드 PASS. 전체 T-05·T-10·T-29나 전체 HTTP/보안 검사의 통과가 아니다. 별도 결과 파일은 없으며 위 monitor 출력이 증빙이다.

### 2026-11-20 요청 라인 서버 연결 검증

- 변경: `webserver.c`는 고정 8,192바이트 버퍼에 최대 1,024바이트씩 누적 수신해 파서를 호출한다. 정상 라인의 소비 구간만 앱에 넘겨 기존 200을 선택하고, 파서 오류는 400/414, 완성 전 EOF는 빈 입력도 400이다. `http.c`·`http.h`에 빈 오류 응답 상수를 추가했다.
- 테스트: `tests/test_webserver.c`는 실제 handle_client에 recv/send만 제어하고 앱의 200 선택 계약을 보존한 stub을 사용한다. 부분 수신·CR/LF 분할·2,000/8,192바이트·오류/EOF·EINTR·부분 송신·MSG_NOSIGNAL을 검사한다. 앱에 넘긴 바이트는 호출 안에서 복사해 수신 버퍼 수명 종료 뒤 참조하지 않는다.
- 독립 테스트 작성: implementation 역할, 실제 모델 `opencode-go/kimi-k2.7-code` / Kimi K2.7 Code, 작업 `st_01a1f89c`, COMPLETED. 주 담당자가 테스트를 검토하고 수신 mock의 미소비 바이트 보존·앱 stub의 버퍼 수명·Python 준비 이벤트와 오류 응답 헤더 검사를 보완했다.
- 전환 전: 위 작업자가 `docker.exe exec week13-web-server-dev make test-unit` 실행, make exit 2. 기존 구현이 파서 소비 길이가 아닌 원시 수신 전체를 앱에 넘겨 test_normal_valid에서 실패했다. 후속 사례는 첫 실패 때문에 실행되지 않았다.
- 전환 후 단위: 같은 명령, monitor `mon_RQWZF8V8QYX6ZMAH`, exit 0. `HTTP_REQUEST_LINE_TESTS_PASS`, `WEBSERVER_REQUEST_LINE_TESTS_PASS`. Input/output error 진단은 EIO 수신 실패의 의도적 주입이다.
- 실제 소켓: 별도 `docker.exe run --rm --init --network none --user 1000:1000 --env HOME=/tmp --mount "type=bind,source=$(wslpath -w "$PWD"),target=/workspace" --workdir /workspace week13-web-server-dev:trixie make test-integration`. monitor `mon_1SWBD60XNNRVKBQG`, exit 0. 엄격한 서버 빌드와 `INTEGRATION_TESTS_PASS`. 정상 경로·미구현 메서드 200, 문법 오류 400, 정확한 8,192바이트 200·8,193바이트 414, 미완성/빈 EOF 400, 후속 정상 요청을 검사했다.
- 실제 응답 검증: Python 표준 HTTPResponse로 HTTP/1.1·기대 상태·Content-Length: 0·Connection: close·빈 본문을 확인했다. stdout 준비 이벤트 뒤 연결하며 출력 스레드를 계속 비우고 종료 후 자식 프로세스를 회수한다. 송신 호출 분할을 실제 recv 분할의 증거로 사용하지 않는다.
- 메모리: monitor `mon_Q22TEZ8V4N6T21KP`, exit 0. `make test-unit CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer"`로 두 단위 테스트 PASS. 별도 일반 빌드의 서버 연결 단위를 Valgrind의 full leak check·error-exitcode=1로 검사해 종료 시 0바이트·0블록, 오류 0건을 확인했다. 임시 실행 파일은 정리했다.
- 제약: clangd·basedpyright가 없어 LSP는 NOT_RUN이며 GCC와 실제 Python 실행으로 검사했다. 요청 전체·지원 버전/메서드·URI·라우팅·헤더/본문·시간 제한·Date·전체 보안/메모리 검사 통과가 아니다. 고정 포트 소켓 검사는 사용 중인 개발 서버와 분리한 컨테이너에서 수행했다. 별도 결과 파일은 없고 위 monitor 출력이 증빙이다.

### 2026-11-20 헤더 구문 파서 검증

- 변경: `http.c`·`http.h`의 독립 `http_parse_headers()`와 결과 타입, `tests/test_http.c`의 헤더 단위 테스트. 서버는 아직 요청 라인만 파싱하며 기존 응답 동작을 유지한다.
- 환경: 실행 중인 `week13-web-server-dev`의 `/workspace`, Linux/amd64 개발 이미지. clangd가 없어 LSP는 NOT_RUN이며 GCC의 C17·`-Wall -Wextra -Wpedantic -Werror`로 검사했다.
- RED: `docker.exe exec week13-web-server-dev make test-unit`, monitor `mon_QFD36G6HP5A5DAKG`, exit 2. 헤더 함수·타입·한도 상수가 없어 테스트 컴파일이 실패했다.
- GREEN: 같은 명령, monitor `mon_QWCYP666Z1X0VDTZ`, exit 0. `HTTP_REQUEST_LINE_TESTS_PASS`·`HTTP_HEADERS_TESTS_PASS`·`WEBSERVER_REQUEST_LINE_TESTS_PASS`. EIO 진단은 기존 수신 실패 주입의 예상 출력이다.
- 사례: 여러 필드·원본 참조·OWS 제거·빈 값·이름 대소문자/중복 순서·값의 0x80/0xff·본문 비소비, 모든 누적 분할 길이, 잘못된 이름/콜론/CRLF/obs-fold·NUL/제어문자, 필드 줄 8,191/8,192/8,193바이트·99/100/101개·총량 직전/최대/초과. 총량은 요청 라인 16/8,192바이트와 종료 빈 줄을 포함하고 본문을 제외한다.
- 메모리: monitor `mon_JTF11C2F2QFDRA3D`, exit 0. `make test-unit CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer"` PASS. 이어 같은 엄격한 옵션의 일반 디버그 빌드로 `tests/test_http.c http.c`를 임시 실행 파일로 만들고 `valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1`로 검사했다. 오류 0건, 종료 시 0바이트·0블록이며 임시 파일은 trap으로 정리했다.
- 서버 회귀: 별도 `docker.exe run --rm --init --network none --user 1000:1000 --env HOME=/tmp --mount "type=bind,source=$(wslpath -w "$PWD"),target=/workspace" --workdir /workspace week13-web-server-dev:trixie make test-integration`, monitor `mon_471PE4SYNQ1CXT91`, exit 0. 엄격한 서버 빌드·기존 `INTEGRATION_TESTS_PASS`.
- 제약: 위 소켓 검사는 기존 요청 라인 동작의 회귀 검사이며 헤더 수신 통합 검사가 아니다. Host·CL·TE 등 의미 검증·본문·전체 HTTP/보안·FD·성능 검사는 NOT_RUN이다. 별도 결과 파일은 없고 위 monitor 출력이 증빙이다.
- 명세 확인 보조: explore 역할, 실제 모델 `opencode-go/deepseek-v4.1-flash`, 작업 `st_01a1f8b5`, CANCELLED. 최종 결과 전달 전에 주 담당자가 관련 명세를 직접 확인해 구현·검증을 끝냈으므로 중복 조사를 중지했다. 작업자의 미완료 결과를 증빙으로 사용하지 않았다.

### 2026-11-20 본문 파서와 세 파서 수신 연결 검증

- 변경: `http_body.c`·`http.h`의 `http_body_init()`·`http_parse_body()`·`http_body_destroy()`, 요청 구조체와 상태 타입. `webserver.c`는 요청 라인·헤더·본문을 연결하고 `app.c`의 `app_handle_request()`에 완료 요청을 한 번 전달한다. 앱은 원문 대신 헤더 개수·본문 길이만 출력하고 빈 200을 반환한다. Makefile에 본문 소스와 `tests/test_body.c`를 연결했다.
- 입력·소유권: 요청 라인·헤더는 누적 버퍼, 본문은 새 바이트만 입력한다. 본문 NEED_MORE에서도 consumed가 유효하다. 원본 head 버퍼와 디코딩 본문은 앱 콜백 동안만 유효하며 완료·오류·수신/송신 실패에서 본문 메모리를 해제한다. 남은 입력은 다음 요청으로 실행하지 않는다.
- 환경: 기존 Linux/amd64 개발 이미지·비특권 사용자, `/workspace`. 단위·메모리는 `docker.exe exec week13-web-server-dev`, 소켓/curl은 사용 중인 서버와 분리한 `docker.exe run --rm --init --network none --user 1000:1000 --env HOME=/tmp --mount "type=bind,source=$(wslpath -w "$PWD"),target=/workspace" --workdir /workspace week13-web-server-dev:trixie`에서 실행했다.
- RED: `make test-unit`, monitor `mon_AX5QZQ9XA8Q070ZG`, exit 2. 기존 라인/헤더 테스트는 PASS, 새 본문 함수·타입 미선언으로 컴파일이 실패했다.
- 본문 GREEN·서버 빌드: monitor `mon_QEKYWSD2QBDD4H87`와 `mon_DR978M9RFGVQ7Y6T`, 모두 exit 0. 본문 단위 PASS와 네 C 소스의 엄격한 `make` 빌드. 후자는 확장/trailer 누적 한도·40,000개 작은 청크 테스트도 포함한다.
- 최종 전체 단위: `docker.exe exec week13-web-server-dev make test-unit`, monitor `mon_72K9GWHAH27NZCQT`, exit 0. `HTTP_REQUEST_LINE_TESTS_PASS`·`HTTP_HEADERS_TESTS_PASS`·`HTTP_BODY_TESTS_PASS`·`WEBSERVER_BODY_FRAMING_TESTS_PASS`.
- 실제 소켓: 위 격리 컨테이너에서 `make test-integration`, monitor `mon_8MNQTQD4NWKE2YN5`, exit 0, `INTEGRATION_TESTS_PASS`. 바이너리 CL/chunked·각 1 MiB·초과 선언·중복/잘못된 프레이밍·미완성 라인/헤더/본문/trailer EOF·400/413/414/417/431/501·후속 정상 연결과 본문 전 실제 100 왕복을 검사했다.
- 실제 curl: 같은 격리 컨테이너·읽기 전용 마운트에서 Python 표준 하네스로 서버를 시작/회수하고 `curl --silent --show-error --include --max-time 5 --data-binary @-`를 실행했다. 0~255 바이너리 입력의 기본 CL·`Transfer-Encoding: chunked`·`Expect: 100-continue`를 각각 검사했다. monitor `mon_5ETRNVNZ5NDKTYQJ`, exit 0, `CURL_CL_PASS`·`CURL_CHUNKED_PASS`·`CURL_EXPECT_PASS`.
- 메모리: 본문 단독 ASan/UBSan `mon_V7VXE250DPMDGVZH`, Valgrind `mon_52TPT5DMEGTZ8274`, 모두 exit 0. 최종 전체 `make test-unit CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer"`와 서버 연결 C 단위의 일반 디버그 빌드·Valgrind는 `mon_JPYHJW4YQYBX79MN`, exit 0. Valgrind는 `--leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1`을 사용했고 본문/서버 각각 종료 시 0바이트·0블록, 오류 0건이다. 임시 실행 파일은 trap으로 제거했다.
- 결정적 분할: C 서버 단위는 원본 recv/send만 제어하며 미소비 바이트를 보존한다. 전체 chunked 요청의 모든 단일 분할점에서 요청 필드·디코딩 바이너리를 앱 콜백 안에서 복사해 검증한다. 실제 소켓 테스트는 send 횟수를 recv 분할 증거로 쓰지 않으며 준비 출력 이벤트와 bounded I/O로 동기화한다. sleep·재시도는 사용하지 않았다.
- 실패 주입: EIO 수신 실패와 EPIPE 송신 실패 진단은 예상 출력이다. 본문이 이미 할당된 수신 실패·100 송신 실패·최종 송신 실패에도 해제하고, 미완성 요청에는 앱을 호출하지 않는지 확인했다.
- 테스트 초안 보조: implementation 역할, monitor에 노출된 실제 모델 `kimi-k2.7-code`, 작업 `st_01a1f8c1`, CANCELLED. C 테스트 초안이 작성된 상태에서 주 담당자가 인계받았다. 잘못된 CL 바이너리 길이·EOF 스크립트·헤더 초과 fixture·gzip 오류 예상값·길이 0의 NULL memcpy를 바로잡고, 모든 분할·100 생략·할당 후 I/O 실패 테스트와 Python 소켓 테스트를 직접 완성했다. 작업자 완료나 작업자 검증을 주장하지 않는다.
- 제약: clangd·basedpyright가 없어 LSP는 NOT_RUN, GCC·실제 Python으로 검사했다. Host·지원 버전·URI·라우팅·echo 응답·Date·마감·읽지 않은 입력의 응답 보존/닫기 예산·전체 보안/FD/성능 검사는 미구현 또는 NOT_RUN이다. 이번 통과는 요청 수신/프레이밍/파싱 하위 범위이며 전체 Phase 1 완료가 아니다. 별도 결과 파일은 없고 위 monitor 출력이 증빙이다.

### 2026-11-20 HTTP 구현 파일 통합

- 사용자 요청: `http_body.c`를 `http.c`로 합치고 요청 라인·헤더·본문에 쓰는 함수 순서로 배치한다. 마지막에 응답 상수를 둔다. 함수 내용·공개 API·파서 상태·오류/제한 계약은 변경하지 않았다.
- 변경: 본문 함수들을 그대로 이동하고 `http_body.c`를 제거했다. Makefile의 서버·본문 단위·서버 연결 단위 링크 입력에서 삭제한 파일을 뺐다. README와 세 명세의 현재 배치를 동기화했다. 위 본문 구현 검증의 `http_body.c`와 네 C 소스 빌드는 당시 배치의 기록이며 현재 명령으로 해석하지 않는다.
- 이동 대조: 수정 전 두 소스의 읽은 내용을 보관해 통합 소스와 직접 대조했다. include·구역 주석·본문 이동 외 차이 없음, `HTTP_MERGE_CONTENT_MATCH`. 회귀 테스트 파일을 새로 만들지 않았다.
- 단위·sanitizer: `docker.exe exec week13-web-server-dev sh -c 'set -eu; make test-unit; make test-unit CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer"'`, monitor `mon_H5MM1MKM5ZYDKM81`, exit 0. 두 실행 모두 요청 라인·헤더·본문·서버 수신 테스트 PASS. EIO/EPIPE 진단은 기존 실패 주입의 예상 출력이다.
- 빌드·실제 소켓: 기존과 같은 격리 Linux 컨테이너(`--network none`, 비특권 사용자, `/workspace` 소스 마운트)에서 `make test-integration`, monitor `mon_608JFJQE41A0Z3P3`, exit 0. 세 소스 `app.c webserver.c http.c`의 엄격한 빌드와 `INTEGRATION_TESTS_PASS`.
- 제약: 파일 배치 변경 검증이다. clangd가 없어 LSP는 NOT_RUN이며 Valgrind·성능·FD 검사는 이번 이동에서 다시 실행하지 않았다. 위 본문 구현 때의 결과를 이번 실행 결과로 복사하지 않는다. 미구현 서버 기능의 상태도 그대로다.

### 2026-11-20 Router 선행 head 의미 검증

- 문서 점검: 기존 P1-05에는 HTTP/1.1 Host 오류 400만 확정되어 있었고 지원 버전,
  request-target 형식·빈 Host·조합별 우선순위는 OPEN이었다. P1-19의 한 번 디코딩과
  query 분리 요구를 URI 구문 검증과 구별해 세 명세·README·D-04를 동기화했다.
- 구현: `http.c`·`http.h`에 `http_validate_request_head()`와 `http_target`,
  505 고정 응답을 추가했다. `webserver.c`는 헤더 완료 뒤 이 함수를 호출하고
  성공한 구성요소를 앱에 전달한다. 메서드 분기/Router와 파일 경로 디코딩은 추가하지 않았다.
- RED: `docker.exe exec week13-web-server-dev make test-unit`,
  monitor `mon_NJWSNTKZVQ3F4W2C`, exit 2. Host 없는 Expect 요청에서 최종 거부 대신
  추가 recv를 시도하여 `mock.recv_next < mock.recv_calls` assert가 실패했다.
- GREEN: 같은 명령, monitor `mon_ZE5HTSPNR6S91T7P`, exit 0.
  이후 absolute 요청의 모든 분할점과 앱 전달 구성요소 검사를 추가했다.
- 최종 단위·sanitizer: `docker.exe exec week13-web-server-dev sh -c 'set -eu; make test-unit; make test-unit CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer"'`,
  monitor `mon_VNYYPRFS92ZN6R9Z`, exit 0. 두 실행 모두 요청 라인·헤더·head 의미 검증·
  본문·수신 연결 테스트 PASS. EIO/EPIPE 진단은 기존 의도적 실패 주입이다.
- 빌드·실제 소켓: `docker.exe run --rm --init --network none --user 1000:1000 --env HOME=/tmp --mount "type=bind,source=$(wslpath -w "$PWD"),target=/workspace" --workdir /workspace week13-web-server-dev:trixie make test-integration`,
  monitor `mon_70QBQQ0DE5QT0FA4`, exit 0, 엄격한 서버 빌드와 `INTEGRATION_TESTS_PASS`.
  실제 400/505, 높은 minor·absolute 수용, 본문/EOF 없는 Expect 요청의 즉시 최종 거부,
  기존 바이너리 본문·100 왕복·후속 연결을 확인했다.
- Valgrind: `docker.exe exec week13-web-server-dev`에서 `tests/test_http.c http.c`와
  `tests/test_webserver.c http.c`를 각각 C17·엄격한 경고·디버그 옵션으로 임시 빌드했다.
  서버 단위에는 `-Wl,--wrap=recv -Wl,--wrap=send`를 사용했다.
  `valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1`,
  monitor `mon_CY1TMAB6BEFKQ5VC`, exit 0. 두 실행 모두 오류 0건·종료 시 0바이트/0블록,
  임시 실행 파일은 trap으로 제거했다.
- 검증 상태: head 의미 검증 하위 범위 VERIFIED. LSP는 clangd·basedpyright 미설치로
  NOT_RUN이며 설치하지 않았다. 전체 HTTP·파일 경로 안전성·라우팅·Date/오류 본문·
  마감·FD·성능 검사는 이번 결과에 포함하지 않는다. 별도 결과 파일 없이 monitor가 증빙이다.
- 보조 문서 점검: explore 역할, 실제 표시 모델 `opencode-go/deepseek-v4.1-flash`,
  작업 `st_01a1f910`, CANCELLED. 최종 결과가 전달되기 전에 주 담당자의 문서 대조와
  검증이 끝나 중복 작업을 중지했다. 독립 검토 완료나 보조 작업자의 검증을 주장하지 않는다.

### 2026-11-20 고정 index/echo Router 검증

- 변경: `router.c`·`router.h`의 한 번 디코딩/경로/메서드 선택, `app.c`·`app.h`의
  index/echo/Router 오류 응답, `http.c`·`http.h`의 응답 구조체·헤더 생성,
  `webserver.c`의 바이트/파일 송신·HEAD 억제·본문 전 거부와 FD 정리.
  루트 `index.html`과 Router C 테스트를 추가하고 Makefile·세 명세·README를 동기화했다.
- 초기 RED: 새 Router C 테스트가 `router.h` 부재로 실패했다.
  monitor `mon_WY3XM3D5X1HZH6JZ`, exit 1.
- 초기 GREEN: Router 단독 `mon_5827CG6CFYFYSYGP`, 전체 단위/빌드
  `mon_X9BF5JWB65M328N4`, 준비한 파일 FD의 실패 경로 추가 후 `mon_JN5XPV6M6AR5W542`,
  모두 exit 0. 초기 sanitizer/Valgrind `mon_SZVMRK4FS5XPPEN3`도 exit 0.
- 실제 curl: 읽기 전용 소스·격리 루프백의 비특권 임시 컨테이너에서 임시 서버를 엄격히
  빌드하고 시작/회수했다. index 응답은 `cmp index.html -`로, echo는 0~255 전체 바이트와
  가짜 HTTP 헤더 본문을 비교했다. `mon_GF0HPPQXWMSBTK7R`, exit 0,
  `CURL_INDEX_PASS`·`CURL_ECHO_CL_PASS`·`CURL_ECHO_CHUNKED_PASS`·`CURL_ECHO_EXPECT_PASS`.
- 소켓 테스트 보조: implementation 역할, 실제 모델 `Kimi K2.7 Code`,
  작업 `st_01a1f921`, COMPLETED. Python 테스트 초안을 작성하고 당시 단위/소켓을 통과했다.
  주 담당자는 전체 파일을 읽고 테스트의 원본 index 이동을 임시 cwd fixture로 바꾸고,
  EOF 없이 조용한 시간 초과도 통과하던 HEAD 검사를 실제 EOF 확인으로 보완했다.
  버전/absolute-form·중복 디코딩·head/용량 오류의 100 이전 거부 회귀를 복원/추가하고
  HTML/script/가짜 응답·요청 헤더와 Content-Encoding의 비반영도 검사했다.
  이 보완 뒤 소켓 `mon_MPJAYAM3VC5QY00V`, exit 0.
- 코드 검토 보조: analysis 역할, 실제 모델 `DeepSeek V4.1 Flash`,
  작업 `st_01a1f926`, COMPLETED. Unix 소켓 index의 ENXIO가 500으로 분류되는 결함과
  비어 있지 않은 응답 Content-Length/405 Allow의 C 단위 검증 공백을 지적했다.
  주 담당자가 Unix 소켓 fixture를 추가해 `mon_K2Y0MA11PGBSXKZ8`, exit 134로 재현했다.
  app의 ENXIO/ENODEV를 403으로 분류하고 Unix 소켓의 실제 소켓/단위 회귀와
  비어 있지 않은 바이트/파일/HEAD의 길이, 405 Allow 송신 검사를 추가했다.
- 최종 단위·메모리: `docker.exe exec week13-web-server-dev`에서 `make test-unit`,
  `make test-unit CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer"`,
  Router/서버 연결 C 단위의 일반 디버그 빌드·Valgrind를 순서대로 실행했다.
  Valgrind는 `--leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1`,
  서버 단위는 `-Wl,--wrap=recv -Wl,--wrap=send`, Router 단위는 `router.c`를 링크했다.
  `mon_9167HBTNFA6V2H4P`, exit 0. 여섯 테스트 그룹 PASS,
  Valgrind 두 실행 모두 오류 0건·종료 시 0바이트/0블록. 임시 실행 파일은 trap으로 제거했다.
  EIO/EPIPE와 조기 파일 EOF 진단은 의도한 실패 주입이다.
- 최종 빌드·소켓: `docker.exe run --rm --init --network none --user 1000:1000 --env HOME=/tmp --mount "type=bind,source=$(wslpath -w "$PWD"),target=/workspace" --workdir /workspace week13-web-server-dev:trixie make test-integration`,
  `mon_2MYY1MYQN0082GC8`, exit 0, `INTEGRATION_TESTS_PASS`.
  index/HEAD/query·한 번 디코딩·404/405/501·바이너리/1 MiB/빈 CL·chunked echo·
  프레이밍/EOF/한도·100 왕복·head/Router/파일 오류의 100 이전 거부·후속 연결을 검사했다.
  index 없음·symlink·FIFO·Unix 소켓은 임시 cwd에서만 만들고 서버 종료 후 제거한다.
- 제약: 고정 두 경로 하위 범위 VERIFIED이며 전체 Phase 1의 완료가 아니다.
  CLI root·일반 정적 파일/링크 허용·마감·접근 로그·전체 FD/성능 검사는 미구현/NOT_RUN이다.
  clangd/basedpyright/biome 미설치로 LSP는 NOT_RUN이다.
  Bun.WebView의 Chrome 실행 실패로 브라우저 렌더는 NOT_RUN이며 설치하지 않았다.
  별도 결과 파일 없이 위 monitor 출력이 실행 증빙이다.

### HTTP 응답 통합 검증 (2026-11-21)

- 범위: 빈 고정 파싱/프레이밍 오류를 공통 표현 응답으로 교체했다.
  `http.c`는 상태 사유와 헤더, `webserver.c`는 같은 최종 송신·HEAD 억제,
  `app.c`는 파일 오류의 내부 진단을 담당한다. 100 Continue만 별도 고정 바이트다.
- RED: 실제 소켓 `mon_6YQWCP6C1XQFY3Y3`, exit 1.
  `test_head_parser_errors`의 잘못된 HEAD 헤더에서 Content-Type 누락을 재현했다.
  C 보조 작업은 408 사유 미지원도 단위 테스트에서 먼저 재현했다.
- 최종 생산 코드 검증: 읽기 전용 소스 bind mount·비특권 사용자·격리 루프백의
  `week13-web-server-dev:trixie` 임시 컨테이너에서 `make test-unit`,
  `gcc -std=c17 -Wall -Wextra -Wpedantic -Werror app.c webserver.c http.c router.c -o "$b"`,
  `python3 tests/test_server.py "$b"`,
  `make test-unit CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer"`
  실행. `mon_P7SV3Z2ZXA7HENR6`, exit 0.
  아홉 단위 그룹·`INTEGRATION_TESTS_PASS`·`HTTP_RESPONSE_FINAL_PASS`를 확인했다.
  임시 실행 파일과 컨테이너는 종료 시 제거했다.
- 408 헤더/500 본문 단위 검사를 보강한 뒤 같은 환경에서 일반·ASan/UBSan
  `make test-unit`을 다시 실행하고, 네 C 단위 실행 파일을 디버그 빌드해
  `valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1`
  검사했다. 서버는 recv/send, Router는 open/fstat linker 래핑을 사용한다.
  `mon_CWTJYKV8RAD8CHXY`, exit 0, `HTTP_RESPONSE_MEMORY_PASS`.
  네 실행 모두 종료 시 0바이트/0블록·오류 0건이며, 실패 주입의 EIO/EPIPE 진단은 예상 결과다.
- 실제 소켓 추가 범위: 파싱/프레이밍 오류의 Date·타입·정확한 길이·고정 본문,
  HEAD의 헤더 구문/Host/버전/CL/TE/Expect/431/본문/EOF 오류에서 실제 본문 0바이트,
  100 뒤 잘못된 chunk의 공통 400 최종 응답. 기존 index/echo·405 Allow·바이너리 회귀도 유지한다.
- 통제된 500: Router C 테스트에서 GNU linker의 open/fstat 래핑으로 EIO를 주입한다.
  생산 코드에 테스트 분기를 넣지 않으며 상태·타입·고정 본문을 검사한다.
  실제 소켓에서 OS 자원 실패를 유발한 500 검사는 이 증빙에 포함하지 않는다.
- 구현 보조: implementation, 실제 모델 `Kimi K2.7 Code`, `st_01a1fbf7`, COMPLETED.
  C 코드·단위·실패 주입을 작성했고 주 담당자가 직접 읽고 소켓 테스트·408 헤더/500 본문 검사를 보강했다.
- 독립 검토: explore, 실제 모델 `DeepSeek V4.1 Flash`, `st_01a1fc03`, COMPLETED.
  공통 응답·HEAD·중복 최종 송신·FD 정리 경로에서 구체적인 생산 코드 결함을 발견하지 않았다.
  408의 실제 마감 경로는 의도한 미구현 범위로 구분했다.
- 제약: clangd/basedpyright 미설치로 LSP는 NOT_RUN이며 설치하지 않았다.
  실제 시간 초과/408·송신/닫기 예산, 일반 root·CLI·접근 로그·전체 FD/부하 검사는 별도 범위다.
  현재 응답 생성의 완료를 전체 Phase 1 완료로 간주하지 않는다.

## 9. 다음 작업과 재개 지점

1. 요청 파싱·head 검증에 이어 고정 index/echo Router를 구현했다. query 무시·한 번 디코딩·메서드/경로 오류와 Expect 순서는 위 하위 계약을 따른다. 다음 확장은 일반 정적 루트/CLI·파일 경계·마감의 관련 결정을 먼저 닫은 뒤 진행한다. 전체 Phase 1 승인은 아직 없다.
2. CLI는 이미 확정한 이름·필수 인자·포트 범위를 유지하고 남은 D-07 하위 결정과 D-02의 루트 초기화 정책을 정한다.
3. 해당 계약이 DECIDED인 범위부터 서버 코드와 실제 성공/실패 검증을 작성한다. 개발환경 검사를 서버 테스트로 재사용하지 않는다.
4. 요청 라인의 구문 분리와 서버 지원 판정은 별개다. D-04의 버전·대상 구문·Host 및 D-03의 초과 코드·trailer 하위 계약은 결정·구현했다. 이것으로 파일 경로 정규화나 Router 지원을 승인하지 않는다. P1-11 한도는 확정 수치를 유지한다.
5. Phase 1의 소켓 마감 전에 D-05, 정적 파일 전에 D-01·D-02, 로그·전체 검사 전에 D-06·D-08의 관련 하위 결정을 닫는다. 정상 종료·시그널 관련 하위 결정은 Phase 2 시작에 닫고 P1-27/T-27을 구현한다.
6. 각 구현 단위의 실제 결과를 갱신한다. 모든 D 그룹의 VERIFIED를 첫 빌드의 착수 조건으로 요구하지 않는다.

새 대화에서 시작할 위치를 찾으려고 전체 인터뷰를 다시 할 필요는 없다.
원장·세 명세·변경된 실제 파일을 읽고 열린 항목부터 이어 간다.
