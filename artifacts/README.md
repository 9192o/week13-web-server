# 검증 산출물

`artifacts/`는 실행한 검증의 증빙을 보관하는 곳이다. 서버 소스, 테스트 코드,
정적 공개 파일은 여기 두지 않는다. 검증 기준과 실행 절차는 `docs/`에서 관리한다.

| 보관 자료 | 내용 |
| --- | --- |
| [Phase 1 실행 기록](phase1/linux-amd64/2026-11-22/README.md) · [벤치마크 JSON](phase1/linux-amd64/2026-11-22/benchmark.json) | Linux Docker 순차 서버 기준선 |
| [Phase 2 검증 문서](../docs/PHASE_2_TESTING.md) · [벤치마크 JSON](phase2/benchmark.json) | 순차 서버 / LT `epoll` 새 연결 / LT `epoll` keep-alive 비교. 기준선의 연결 리셋 1건을 포함한 원본 |

기존 파일은 당시 실행 결과다. 벤치마크를 다시 돌릴 때는 별도 출력 경로를 사용하고,
비교나 제출 증빙으로 보관할 실행만 버전·환경·명령·종료 코드와 함께 남긴다.
