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
16개 seed 기준 60초 smoke 실행:

```text
Done 2274442 runs in 61 second(s)
0 crashes
```

이 결과는 parser fuzz smoke test이며, 전체 서버 동작·프로토콜 적합성·
커버리지를 보증하지 않는다. CI 상시 작업이 아니라 필요할 때 실행하는 보조 검사다.

## Seed corpus

`tests/fuzz_corpus/`에는 정상 요청, chunked, CL/TE 충돌, 중복 CL, absolute-form,
chunked response, 1xx+final, 204/205/304, EOF-delimited response, trailer response를
포함한다.
