#!/bin/sh
# Verify an exact committed tree on the container's POSIX filesystem.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    printf 'Usage: sh tests/verify_phase1.sh {test|memory|benchmark|environment} [revision]\n' >&2
    exit 2
fi

workspace=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
cd "$workspace"
revision=$(git rev-parse --verify "${2:-HEAD}^{commit}")
docker_cli=docker
if [ -n "${WSL_DISTRO_NAME:-}" ]; then
    docker_cli=docker.exe
fi
image=$("$docker_cli" image inspect week13-web-server-dev:trixie --format '{{.Id}}' | tr -d '\r')
test -n "$image"
printf 'Source: %s\nImage: %s\n' "$revision" "$image" >&2
"$docker_cli" version --format 'Docker client={{.Client.Version}} server={{.Server.Version}}' >&2

case "$1" in
    test) command='make -B test' ;;
    memory) command='make sanitize; make valgrind' ;;
    benchmark) command='make benchmark' ;;
    environment) command='
        uname -rsmo
        id
        stat -f -c FS=%T .
        ulimit -n
        for resource in cpu.max memory.max pids.max; do
            printf "%s=" "$resource"
            head -n 1 "/sys/fs/cgroup/$resource"
        done
        make dev-check
        ' ;;
    *) printf 'Unknown verification mode: %s\n' "$1" >&2; exit 2 ;;
esac

archive=$(mktemp)
trap 'rm -f "$archive"' EXIT
trap 'exit 1' HUP INT TERM
git archive --format=tar "$revision" >"$archive"
"$docker_cli" run --rm --init -i --platform linux/amd64 --network none \
    --user 1000:1000 --env HOME=/tmp --cpus 2 --memory 512m --pids-limit 128 \
    --ulimit nofile=256:256 "$image" sh -ec \
    "mkdir /tmp/phase1; cd /tmp/phase1; tar -xf -; $command" <"$archive"
