#!/bin/sh
set -eu

usage()
{
    printf 'Usage: sh dev.sh {build|shell|check}\n'
}

if [ "$#" -gt 1 ]; then
    usage >&2
    exit 2
fi

action=${1:-help}
image=week13-web-server-dev:trixie
workspace=$(CDPATH= cd "$(dirname "$0")" && pwd)
docker_cli=docker
mount_source=$workspace
if [ -n "${WSL_DISTRO_NAME:-}" ]; then
    docker_cli=docker.exe
    mount_source=$(wslpath -w "$workspace")
fi

case "$action" in
    build)
        cd "$workspace"
        exec "$docker_cli" build --platform linux/amd64 --tag "$image" .
        ;;
    shell|check)
        set -- --rm --init --platform linux/amd64 \
            --user "$(id -u):$(id -g)" \
            --env HOME=/tmp \
            --mount "type=bind,source=$mount_source,target=/workspace" \
            --workdir /workspace \
            --stop-timeout 35 \
            "$image"
        if [ "$action" = shell ]; then
            exec "$docker_cli" run --interactive --tty "$@" /bin/bash
        fi
        exec "$docker_cli" run "$@" make dev-check
        ;;
    help)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
