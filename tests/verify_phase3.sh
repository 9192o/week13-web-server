#!/bin/sh
# Verify the current source snapshot without changing the working tree.
set -eu

cd "$(dirname "$0")/.."
docker_cli=docker
if [ -n "${WSL_DISTRO_NAME:-}" ]; then
    docker_cli=docker.exe
fi
image=$("$docker_cli" image inspect week13-web-server-dev:trixie --format '{{.Id}}' | tr -d '\r')
test -n "$image"
printf 'Image: %s\n' "$image" >&2

snapshot=$(mktemp)
trap 'rm -f "$snapshot"' EXIT
trap 'exit 1' HUP INT TERM
tar -cf "$snapshot" Makefile src tests examples public index.html
"$docker_cli" run --rm --init -i --platform linux/amd64 --network none \
    --user 1000:1000 --env HOME=/tmp --cpus 2 --memory 512m --pids-limit 128 \
    --ulimit nofile=1024:1024 "$image" sh -eu -c '
    cd "$(mktemp -d)"
    tar -xf -
    uname -rsmo
    gcc -dumpfullversion
    make -B test test-shutdown test-epoll test-keepalive test-proxy
    echo PHASE3_FUNCTIONAL_PASS
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        make -B test test-shutdown test-epoll test-keepalive test-proxy \
        CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie"
    make valgrind
    make test-proxy-unit CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -g" \
        UNIT_RUNNER="valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=91 --track-fds=yes"
    echo PHASE3_MEMORY_PASS
    ' <"$snapshot"
