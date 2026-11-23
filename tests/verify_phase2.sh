#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    printf 'Usage: sh tests/verify_phase2.sh {test|memory|benchmark} [revision]\n' >&2
    exit 2
fi
case "$1" in test|memory|benchmark) mode=$1 ;; *) exit 2 ;; esac

cd "$(dirname "$0")/.."
revision=$(GIT_MASTER=1 git rev-parse --verify "${2:-HEAD}^{commit}")
baseline=$(GIT_MASTER=1 git rev-parse --verify '5b20a75^{commit}')
docker_cli=docker
if [ -n "${WSL_DISTRO_NAME:-}" ]; then
    docker_cli=docker.exe
fi
image=$("$docker_cli" image inspect week13-web-server-dev:trixie --format '{{.Id}}' | tr -d '\r')
test -n "$image"
printf 'Source: %s\nBaseline: %s\nImage: %s\n' "$revision" "$baseline" "$image" >&2
"$docker_cli" version --format 'Docker client={{.Client.Version}} server={{.Server.Version}}' >&2

files=$(mktemp -d)
trap 'rm -rf "$files"' EXIT
trap 'exit 1' HUP INT TERM
GIT_MASTER=1 git archive --format=tar "$revision" >"$files/phase2.tar"
if [ "$mode" = benchmark ]; then
    GIT_MASTER=1 git archive --format=tar "$baseline" >"$files/phase1.tar"
fi

tar -C "$files" -cf - . |
    "$docker_cli" run --rm --init -i --platform linux/amd64 --network none \
        --user 1000:1000 --env HOME=/tmp --cpus 2 --memory 512m --pids-limit 128 \
        --ulimit nofile=1024:1024 "$image" sh -eu -c '
        work=$(mktemp -d)
        cd "$work"
        tar -xf -
        mkdir phase2
        tar -xf phase2.tar -C phase2
        uname -rsmo >&2
        gcc -dumpfullversion >&2
        case "$1" in
            test)
                cd phase2
                make -B test test-shutdown test-epoll test-keepalive
                gcc -std=c17 -Wall -Wextra -Wpedantic -Werror examples/epoll_modes.c -o "$work/epoll_modes"
                "$work/epoll_modes"
                ;;
            memory)
                cd phase2
                ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
                    make -B test test-shutdown test-epoll test-keepalive \
                    CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie"
                make valgrind
                ;;
            benchmark)
                mkdir phase1
                tar -xf phase1.tar -C phase1
                flags="-std=c17 -Wall -Wextra -Wpedantic -Werror -O2"
                make -C phase1 CFLAGS="$flags" >&2
                make -C phase2 CFLAGS="$flags" >&2
                python3 phase2/tests/benchmark_phase2.py phase1/webserver phase2/webserver \
                    --concurrency 1,8,32 --repeat 3 --samples 15 \
                    --static-sizes 1024,65536,2097152 --echo-sizes 0,1024,65536,1048576 \
                    --log-file /dev/null
                ;;
        esac
        ' sh "$mode"
