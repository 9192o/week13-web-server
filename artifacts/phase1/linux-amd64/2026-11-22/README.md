# Phase 1 Linux Docker 검증 — 2026-11-22

검사한 서버 소스: `762e1d7`. 성능 원자료를 낸 단계별 스테이징 트리:
`acda4e6eacbc258a974f9d8e20aa931b77fe89e5`. 작업 디렉터리는 컨테이너
내 `/tmp/phase1`이다. 소스는 tar로 전송했고, 정적 루트와 테스트 파일은
case-sensitive overlayfs에서 만들었다.

- 호스트: Windows/WSL2, AMD Ryzen AI 7 350. Linux x86_64 커널
  `6.18.33.2-microsoft-standard-WSL2`; Docker client/server 29.9.2.
- 이미지: `week13-web-server-dev:trixie`, Linux/amd64,
  `sha256:6fdaf83a899150c0e1c7dd4e843ea2e45e74d91779d22653f07db8640b8a54df`.
- UID/GID 1000:1000, `--init --network none`, CPU 2개, 메모리 512 MiB,
  PID 128개, FD 256개. 호스트 포트·호스트 디렉터리·Docker 소켓 미공유.
- GCC 14.2.0, Python 3.13.5, curl 8.14.1, Valgrind 3.24.0.
- 호스트 재실행: `sh tests/verify_phase1.sh environment|test|memory|benchmark`.
  CLI가 기대하는 정적 루트는 컨테이너의 검증된 파일 시스템에 둔다.

| 항목 | 컨테이너 명령 | 결과 |
| --- | --- | --- |
| 환경 (T-02/31) | `make dev-check` | PASS(0), `mon_W6ZMJZTQDDXK7CMB`. `DEV_ENV_OK`, 비특권·자원 한도 확인 |
| 기능 및 실행된 경계 사례 | `make -B test` | PASS(0), `mon_F4NKV21WZM2XYKVW`. C 단위, 실제 소켓, curl, 단축 마감·408·JSONL 검증 |
| 메모리 (T-29) | `make sanitize && make valgrind` | PASS(0), `mon_4JV3DR5TYNNRRJ08`. ASan/UBSan 빌드 단위·소켓, Valgrind C 단위 6개 각각 종료 시 0바이트·오류 0 |
| FD (T-26/30) | `make -B test` / `make benchmark` | 실행 대상 PASS. 408 후 정상 요청 30회 모두 기준 FD 복귀; 벤치마크 FD 시작 6, 관측 최고 8, 종료 전 6 |
| 성능 (T-30) | `make benchmark` | PASS(0), `mon_K9Z8JQ6D1DEFFSH7`. [원자료](benchmark.json): 11개 워크로드 × 3회 × 30요청 = 990건 성공, 0건 실패. RSS 시작 1,748,992 B, 관측 최고 3,018,752 B |

Valgrind 첫 시도에서는 단위 테스트가 닫힌 FD에 `fcntl`을 호출하는 확인
방법 때문에 FD 오류 6건이 보고됐다. `/proc/self/fd` 경로의 부재로 같은
정리 조건을 검사하도록 수정한 후 전체 메모리 게이트를 다시 실행했다.

## Phase 1 완료 판정

**미완료 — 전체 명세가 요구하는 모든 사례를 통과한 것은 아니다.**
실제 케이스를 구현한 C/Python 스위트는 모두 통과했다. 다음 증명은
미실행이며 전체 T 그룹을 PASS로 올리지 않는다.

- T-18: 링크 고정 및 경계 거부의 일반 사례는 통과했다. 디렉터리 변경과
  실제 읽기 사이를 제어하는 전체 동시 변경 매트릭스는 검증하지 않았다.
- T-12/T-26/T-28: 송신 및 전체 수신 마감의 모든 상호작용, 실제 소켓에서
  최종 응답 일부 송신 후 중단, 로그 출력 정체와 연결 회복을 함께 보는
  전체 매트릭스는 검증하지 않았다.
- T-16/T-31: 장치 노드·중첩 마운트 전용 검증은 실행하지 않았다.
  다른 Linux 커널·파일시스템에서의 동작은 이 결과의 적용 범위가 아니다.
- Phase 2로 이관된 P1-27/T-27은 미실행이며 Phase 1 완료 차단이 아니다.

위 수치는 지정 프로파일에서의 측정값이다. RPS 합격선이나 다른 환경에
대한 성능 보증으로 사용하지 않는다. 산출물의 로그 목적지 `/dev/null`은
벤치마크 하네스가 서버 출력을 계속 읽고 버린다는 뜻이다.
