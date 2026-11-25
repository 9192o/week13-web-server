# Phase 1–3 통합 검증

기준: Phase 2 커밋 `2785e06`, 구현 브랜치 `feat/phase3/woong`.

## 실행

호스트에서 실행한다.

```sh
sh dev.sh build
sh dev.sh check
sh tests/verify_phase3.sh
```

검증 스크립트는 서버·테스트·픽스처의 현재 스냅샷을 Linux/amd64 Docker로
복사하므로 미커밋 변경도 포함한다. 이전 Phase 커밋을 따로 빌드하는 대신
Phase 3 코드에서 기존 기능까지 검증한다. 호스트에 포트를 공개하지 않고,
별도 네트워크 연결이 없는 컨테이너의 루프백에서 서버와 백엔드를 실행한다.
UID/GID 1000:1000, CPU 2개, 메모리 512 MiB, 프로세스 128개, 열린 파일
1024개로 제한한다. `sh dev.sh check`는 개발환경 도구와 작업 디렉터리
바인드 마운트의 쓰기 권한을 별도로 확인한다.

## 스위트별 범위

| 단계 | 타깃 | 확인 내용 |
| --- | --- | --- |
| Phase 1 | `make test` | C 단위, 실제 HTTP 소켓·curl, 정적 파일과 MIME, 바이너리 echo, 요청 크기·프레이밍·마감 회귀 |
| Phase 2 | `make test-shutdown test-epoll test-keepalive` | SIGINT/SIGTERM, 느린 요청과 독립 연결, 연결 한도와 FD 회수, 재사용과 파이프라인 |
| Phase 3 | `make test-proxy` | 라우트·헤더 단위, 실제 curl과 두 백엔드, 최장 접두사와 미일치 `404`, 접두사별 라운드 로빈, 바이너리 POST와 쿼리, 백엔드 지연 격리, 응답 스트리밍, `502`/`504`, FD 회수와 종료, 실제 C 백엔드 프로세스 재시작 |

Phase 3에서 추적하는 요구사항은 ENV-08, DYN-02, PROXY-01~04, REV-01~06이다.
헤더·마감·연결 정책은 [Phase 3 설계](PHASE_3_ARCHITECTURE.md)에 정리했다.

## 실행 결과 — 라운드 로빈 추가 후, 2026-11-24

`sh tests/verify_phase3.sh` (`mon_R67DHVCEWZVF6BZE`)는 아래와 같은
Linux/amd64 Docker 이미지·커널 및 GCC 14.2.0에서 종료 코드 0이었다.
일반·ASan/UBSan/LSan 스위트 모두에서 실제 백엔드 두 개를 사용해
겹치는 두 접두사가 독립적으로 등록 순서대로 순환함을 확인했다.
Valgrind C 단위 바이너리 8개 모두 오류 0건이었다.

추가 루프백 검사에서 첫 백엔드는 포트를 예약하되 수신하지 않도록 하고
두 번째는 정상 응답하게 했다. 세 요청의 결과가 `502 → 200 → 502`로,
실패를 다른 백엔드 재시도로 숨기지 않고 다음 요청에서 순번을 진행함을 확인했다.
처리량이나 장애 자동 복구 정책을 검증한 것은 아니다.

## 실행 결과 — 실행 파일 분리 후, 2026-11-24

Windows/WSL2 Linux/amd64 Docker, 아래와 같은 이미지·커널, GCC 14.2.0.

| 실행 | 결과 | 증빙 |
| --- | --- | --- |
| `sh tests/verify_phase3.sh` | PASS (0). Phase 1–3 일반 및 ASan/UBSan/LSan 스위트, Valgrind C 단위 바이너리 8개 오류 0건 | `mon_R08XK4ND79NRAV85` |
| 분리된 소스 사본에서 `make reverse-proxy`와 `make webserver` | PASS (0). 프록시 빌드에서 `src/app/`·`src/static/`을, 백엔드 빌드에서 `src/proxy/`를 제외해도 각각 빌드됨 | `mon_8E8J4SDY1PMXQBHK` |

`make test-proxy`의 실제 프로세스 검사는 백엔드 없이 프록시를 시작해 `502`를
확인하고, C 백엔드 기동 뒤 정적 `GET`과 바이너리 `POST /echo`를 중계한다.
백엔드를 중지하면 `502`로 돌아가고, 같은 포트로 재기동하면 프록시 재시작
없이 바뀐 정적 페이지를 반환한다. 이 검사를 분리 전 바이너리로 실행하면
`--root`를 요구해 프록시 기동에서 실패했다(종료 1).

## 이전 실행 결과 — 실행 파일 분리 전, 2026-11-24

대상: `feat/phase3/woong`의 현재 소스 스냅샷(HEAD `2785e06` + 미커밋 Phase 3).
Windows/WSL2 환경의 Docker 29.9.3, Linux/amd64 개발
이미지 `sha256:8542426ab0d1d4b49eced209564c1162aa807d5a9667d0335f786a9b50559bb9`,
Linux `6.18.33.2-microsoft-standard-WSL2`, GCC 14.2.0.

| 실행 | 결과 | 증빙 |
| --- | --- | --- |
| `sh tests/verify_phase3.sh` | PASS (0). 일반 빌드와 ASan/UBSan/LSan 빌드 각각에서 `make -B test test-shutdown test-epoll test-keepalive test-proxy` 통과. Valgrind C 단위 바이너리 8개 모두 오류 0건, 종료 시 할당 0바이트 | `mon_TY4GSETZ5HW64APT` |
| `sh dev.sh check` | PASS (0). 비특권 실행, 바인드 마운트의 쓰기 권한, C17/POSIX 및 메모리 검사 도구 확인 | `mon_2FK1780VZX1QVPTV` |
| [`examples/epoll_modes.c`](../examples/README.md) 컴파일·실행 | PASS (0). `gcc -std=c17 -Wall -Wextra -Wpedantic -Werror`로 빌드. 일부 읽기 후 LT는 다시 알리고 ET는 알리지 않음 | `mon_2FK1780VZX1QVPTV` |

이전 Phase 3 기능·메모리 검사도 같은 이미지에서 종료 코드 0이었다
(`mon_0A8SSGGD8GGJF3QP`). 구현 전 기준 `2785e06`에 프록시 소켓 테스트만
추가했을 때는 `--route` 미지원으로 첫 서버가 준비되지 않아 테스트가
종료 코드 1이었다.

## 이번 판정에 포함하지 않은 항목

- 부하 테스트는 별도다. [Phase 1 기준선](PHASE_1_TESTING.md#6-성능-측정-프로토콜-d-08)과
  [Phase 2 A/B/C 측정](PHASE_2_TESTING.md#abc-성능-측정)의 기존 결과는
  재측정하지 않았다. 현재 프록시 경로의 처리량과 지연은 미측정이다.
- [Phase 1 검증 공백](PHASE_1_TESTING.md#7-실행-결과와-완료-판정)의 전체
  링크·마감·마운트 조합이나 다른 커널·파일 시스템까지 통과한 것은 아니다.
  다만 Phase 2로 이관한 정상 종료(T-27)는 이번 스위트에서 확인했다.
- 브라우저 화면, macOS 네이티브 실행과 독립 코드 리뷰는 이번 결과에 포함하지 않는다.
