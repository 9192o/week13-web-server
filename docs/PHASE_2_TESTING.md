# Phase 2 검증과 재현

이 문서는 [Phase 2 요구사항](PHASE_2_REQUIREMENTS.md)의 실행 증빙을 기록한다.
Phase 1의 역사적 미검증 항목이나 Phase 3의 프록시를 대신 판정하지 않는다.

## Linux Docker 검증

소스 `ed230e3`, Docker 29.9.3, 이미지
`sha256:9efed1380e6f14112be1f169f9075074647aff64f8d87f9c8a87d78577181255`,
Linux `6.18.33.2-microsoft-standard-WSL2` x86_64. `--network none`에서
UID/GID 1000, CPU 2개, 메모리 512 MiB, `nofile=1024`로 실행했다.

| 명령 | 결과 | 증빙 |
| --- | --- | --- |
| `sh tests/verify_phase2.sh test ed230e3` | PASS, exit 0 | `mon_C68NCXNP658P9V05` |
| `sh tests/verify_phase2.sh memory ed230e3` | PASS, exit 0 | `mon_3Z9G9E7F45QA3BNC` |

| 판정 단위 | 확인한 내용 |
| --- | --- |
| T-27 | SIGINT·SIGTERM 각각의 유휴 종료, 미완성 헤더 무응답 종료, 검증된 본문과 진행 중 응답의 완료, 기존 마감 및 단축 정상 종료 유예, 새 연결 미처리 |
| T2-01·02 | 느린 POST와 독립 GET, 연결 한도 2·256과 수락 재개, 종료 시 재개 금지 |
| T2-03~07 | 유휴 종료, CL·chunked 파이프라인 순서, 100개 상한, 오류 후 재사용 여부, 요청당 로그 |
| T2-09 | 반복 연결 중단과 후속 응답, 열린 FD의 유휴 기준 복귀 |

기존 HTTP·정적 파일·라우터·마감 회귀도 통과했다. 메모리 모드에서 단위·소켓
스위트는 ASan/UBSan/LSan으로, C 단위 스위트는 Valgrind로 확인했다.
LT/ET 실습에서는 동일한 일부 읽기 뒤 입력을 더 보내지 않았을 때 LT만 다시 알렸다.

## A/B/C 성능 측정

`sh tests/verify_phase2.sh benchmark ed230e3 > artifacts/phase2/benchmark.json`
(`mon_5KH7VBMQ8VKJWZR6`, exit 1). Phase 1 기준선 `5b20a75`와 Phase 2
`ed230e3`를 같은 격리 컨테이너에서 `-O2`로 빌드했다. 동시 클라이언트
1·8·32, 반복 3회, 정적 GET 3종과 POST `/echo` 4종을 사용했다.
[원본 결과](../artifacts/phase2/benchmark.json)에 63개 측정 행을 남겼다.

| 조건 | 성공 | 실패 |
| --- | ---: | ---: |
| A: Phase 1, 새 연결 | 12,914 | 1 |
| B: Phase 2, 새 연결 | 12,915 | 0 |
| C: Phase 2, keep-alive | 12,915 | 0 |

실패는 A에서 동시 32개, 1 MiB echo 부하 중 관측된 연결 리셋 1건이다.
실패를 제외하거나 재시도로 대체하지 않았다. C의 모든 측정 행에서 연결 재사용을
확인했다. 측정은 완료했지만 벤치마크 명령 자체는 요청 실패로 비정상 종료했다.
이 결과로 어느 연결 방식이 항상 더 빠르다고 결론 내리지 않는다.

## 알려진 범위

- 소켓은 논블로킹이지만 정적 파일 열기와 읽기는 단일 이벤트 루프에서 동기로
  실행한다. 느린 파일 시스템에서 소켓 응답 지연이 없다고 보장하지 않는다.
- 종료 유예 경계 테스트에서는 실제 30초를 기다리지 않고 같은 코드에 단축
  상수를 적용했다. 운영 빌드의 유예 설정은 30초다.

## Phase 2 마감 — 2026-11-23

`ldc`의 불완전 응답 종료 계약을 참고해 T2-09를 보강했다. 전송 중 정적
파일이 짧아지면 서버는 다음 파이프라인 응답을 보내지 않고 연결을 닫아야
하며, 소켓과 파일 FD 수는 유휴 기준으로 돌아와야 한다. 서버 동작은 바꾸지
않았다. `eun/phase1`의 빠른 검사 분리를 참고해 `make test-fast`를 추가했다
(C 단위·기본 소켓 통합, 마감 시나리오 제외).

소스 `272a715`, Linux/amd64 Docker 29.9.3, GCC 14.2.0,
이미지 ID `sha256:e0f456e2f7b68dcd812d2ed9a6949310434d0f2fc98f08ad2a2bdba9157342b2`.
실행 `mon_B4MQ7K6ZBQFT8SAN`에서 `sh dev.sh build`,
`sh tests/verify_phase2.sh test 272a715`,
`sh tests/verify_phase2.sh memory 272a715`가 모두 통과했다(종료 0).
기능 검사에는 종료·epoll·keep-alive·LT/ET 실습이 포함된다. 메모리 검사는
ASan/UBSan/LSan 회귀와 Valgrind C 단위 검사이며, 후자는 오류 0건이었다.

GitHub Actions에는 push/PR용 Docker 기능·메모리 검사를 구성했다.
GitHub 호스티드 러너 실행과 성능 재측정은 이번 마감 검증에 포함하지 않는다.
T2-08의 기존 A/B/C 결과와 Phase 1 기준선에서 관측된 연결 실패 1건은
위에 그대로 남긴다. 과제 필수 프록시는 Phase 3 범위다.
