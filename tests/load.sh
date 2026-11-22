#!/bin/sh
set -eu

PORT="${PORT:-18082}"
LOG="${TMPDIR:-/tmp}/web_server_load.$$.log"

if ! command -v ab >/dev/null 2>&1; then
    echo "SKIP: ab not found (install Apache Bench, wrk, or hey)"
    exit 0
fi

./server "$PORT" ./www >"$LOG" 2>&1 &
pid=$!

cleanup() {
    kill -INT "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

i=0
while [ "$i" -lt 50 ]; do
    if curl -sS --max-time 1 -o /dev/null \
        "http://127.0.0.1:$PORT/" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done

fail=0

run_ab() {
    desc="$1"
    shift
    out="${TMPDIR:-/tmp}/web_server_ab.$$.out"

    if ! ab "$@" >"$out" 2>&1; then
        echo "FAIL: $desc (ab failed)"
        cat "$out"
        fail=1
        rm -f "$out"
        return
    fi

    failed=$(awk '/Failed requests:/{print $3}' "$out")
    non2xx=$(awk '/Non-2xx responses:/{print $3}' "$out")
    rps=$(awk '/Requests per second:/{print $4}' "$out")

    echo "PASS: $desc (failed=${failed:-0} non2xx=${non2xx:-0} rps=${rps:-?})"

    if [ "${failed:-0}" != "0" ]; then
        fail=1
    fi
    if [ -n "${non2xx:-}" ] && [ "$non2xx" != "0" ]; then
        fail=1
    fi

    rm -f "$out"
}

run_ab "ab close-only" -n 1000 -c 1 \
    "http://127.0.0.1:$PORT/index.html"
run_ab "ab keep-alive" -k -n 1000 -c 1 \
    "http://127.0.0.1:$PORT/index.html"

exit "$fail"
