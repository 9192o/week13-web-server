#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

make backend-server reverse-proxy cgi >/dev/null

TMP="${TMPDIR:-/tmp}/web_server_proxy_first.$$"
mkdir -p "$TMP"

cleanup() {
    kill -INT "$PROXY_PID" 2>/dev/null || true
    kill -INT "$BACKEND_PID" 2>/dev/null || true
    wait "$PROXY_PID" 2>/dev/null || true
    wait "$BACKEND_PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

./backend-server --config examples/proxy-first/backend.conf >"$TMP/backend.log" 2>&1 &
BACKEND_PID=$!

./reverse-proxy --config examples/proxy-first/proxy.conf >"$TMP/proxy.log" 2>&1 &
PROXY_PID=$!

sleep 0.5
printf 'client  -> http://127.0.0.1:8080/\n'
printf 'backend -> http://127.0.0.1:8001/ (proxy only)\n'
printf 'Ctrl-C to stop\n\n'

wait
