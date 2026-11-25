# Fuzzing

HTTP parser와 reverse-proxy response parser를 libFuzzer로 검사한다.
네트워크·파일 시스템·이벤트 루프는 대상이 아니며, 순수 파서 robustness 확인용이다.

## Targets

- `http_parse_request_line()`
- `http_parse_headers()`
- `http_validate_request_head()`
- `http_body_init()` / `http_parse_body()` / `http_body_destroy()`
- `proxy_request_head()`
- `proxy_response_head()` with `is_head` true/false

## Fuzz 범위

이 fuzz는 **parser 계층만** 대상으로 한다. 아래 영역은 현재 fuzz에 포함되지 않는다.

| 영역 | fuzz 여부 | 이유 / 필요한 방법 |
| --- | --- | --- |
| HTTP 요청 줄·헤더·본문 parser | 완료 | 현재 `tests/fuzz_http.c` |
| proxy request/response wire parser | 완료 | 현재 `tests/fuzz_http.c` |
| 네트워크 이벤트 루프 | 미포함 | 실제 socket 기반 network fuzz(AFLnet/boofuzz 등) 필요 |
| keep-alive·파이프라인 상태 머신 | 미포함 | 요청 순서/부분 전송을 다루는 stateful network fuzz 필요 |
| proxy exchange 전체(연결·풀·timeout) | 미포함 | mock socket/clock과 backend fault injection 필요 |
| CGI 프로세스·큐 | 미포함 | 동시성 stress, child abort/timeout fault injection 영역 |
| 정적 파일·파일 시스템 경로 | 미포함 | 임시 root fixture와 path fuzz harness가 별도로 필요 |
| 204/205 등 실제 응답 동작 | 미포함 | backend 응답 시퀀스와 framing/hang property assertion 필요 |

따라서 이 fuzz 결과를 **“전체 서버 fuzz 완료”로 해석하면 안 된다.**
전체 서버는 `sh tests/verify_phase3.sh`, 실제 A/B 프록시·CGI E2E, GitHub CI로
검증하며, fuzz는 그중 parser 보조 검사다.

## Run

```sh
sh dev.sh build          # clang + libFuzzer runtime이 포함된 개발 이미지
sh dev.sh shell
make fuzz FUZZ_SECONDS=600
```

`tests/fuzz_corpus/`의 seed를 `fuzz_work/`로 복사한 뒤 실행한다.
libFuzzer가 만든 입력과 timeout/crash artifact는 `fuzz_work/`에만 쓰이며
git에 커밋하지 않는다.

- `FUZZ_SECONDS` 기본값: 60
- `FUZZ_MAX_LEN` 기본값: 1081344 (HTTP body 1 MiB + header 32 KiB)
- `-timeout=2`: 한 입력이 2초를 넘으면 artifact로 저장
- `-fsanitize=fuzzer,address,undefined`

## Result

2026-11-25, `main` `eb486b4`, Linux/amd64 Docker (GCC/Clang 19 이미지),
16개 seed 기준 실행 결과.

60초 smoke:

```text
Done 2274442 runs in 61 second(s)
0 crashes
```

10분 실행:

```text
Done 13627487 runs in 601 second(s)
stat::average_exec_per_sec: 22674
stat::new_units_added:      10952
stat::peak_rss_mb:          532
0 crashes, 0 timeout artifacts
```

이 결과는 parser fuzz이며, 전체 서버 동작·프로토콜 적합성·커버리지를
보증하지 않는다. CI 상시 작업이 아니라 필요할 때 실행하는 보조 검사다.

## Seed corpus

`tests/fuzz_corpus/`에는 정상 요청, chunked, CL/TE 충돌, 중복 CL, absolute-form,
chunked response, 1xx+final, 204/205/304, EOF-delimited response, trailer response를
포함한다.
