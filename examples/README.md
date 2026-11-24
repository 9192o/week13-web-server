# 독립 실습

`examples/`는 서버 동작에 쓰인 OS 개념을 작은 프로그램으로 확인하는 곳이다.
기본 `make`로 빌드하는 웹 서버에는 포함되지 않는다.
[`tests/verify_phase2.sh test`](../tests/verify_phase2.sh)는 서버 테스트와 별도로
아래 실습도 컴파일하고 실행한다.

## `epoll_modes.c` — LT vs ET after a partial read

`epoll` can be used in two notification modes:

* **Level-Triggered (LT, the default):** `epoll_wait` reports a file
  descriptor as long as it is readable. If you read only part of the
  available data, the next `epoll_wait` still reports it.
* **Edge-Triggered (ET):** `epoll_wait` reports a descriptor only when its
  readiness state changes. If you read only part of the available data and
  no new data arrives, the next `epoll_wait` does **not** report it again.

This example demonstrates the difference with two independent `socketpair`
endpoints that receive the identical payload. Each run:

1. registers the read end with `epoll` (LT for the first run, ET for the
   second);
2. writes the payload and waits once for readability without closing the peer;
3. reads exactly one byte, leaving the rest of the payload buffered;
4. performs a second, non-blocking `epoll_wait` with no new input.

Expected output:

```text
LT second wait: event present
ET second wait: event absent
OK: LT reports ready, ET does not after partial read
```

The program exits with status `0` only when LT reports an event on the
second wait and ET does not; otherwise it exits with a non-zero status.

### Linux Docker 개발 셸에서 실행

호스트에서 `sh dev.sh shell`로 개발 셸을 연 다음, 저장소 루트에서 실행한다.
실행 파일은 임시 디렉터리에 만들고 실습이 끝나면 정리한다.

```sh
(
  set -eu
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror examples/epoll_modes.c -o "$work/epoll_modes"
  "$work/epoll_modes"
)
```
