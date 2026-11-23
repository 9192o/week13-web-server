# Phase 2 standalone examples

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

### Compile and run directly in the Linux Docker container

The source tree is mounted read-only at `/workspace`; the working directory
is a temporary path inside the container so the compiled binary is not
written back to the repository.

```sh
docker.exe run \
  --rm \
  --platform linux/amd64 \
  --network none \
  --user 1000:1000 \
  --env HOME=/tmp \
  --mount "type=bind,source=$(wslpath -w "$PWD"),target=/workspace,readonly" \
  --workdir /tmp \
  week13-web-server-dev:trixie \
  sh -c 'gcc -std=c17 -Wall -Wextra -Wpedantic -Werror /workspace/examples/epoll_modes.c -o /tmp/epoll_modes && /tmp/epoll_modes'
```

On a non-WSL Linux host, replace the Docker client path and `wslpath -w`
with the ordinary `docker` command and the native source path.
