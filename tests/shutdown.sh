#!/bin/sh
# Graceful shutdown tests.
#
# Covers:
#   1. idle SIGINT exits cleanly
#   2. idle SIGTERM exits cleanly
#   3. SIGINT during keep-alive: current response completes, next request is not served
#   4. second SIGINT forces exit while a CGI is still running
#
# usage: sh tests/shutdown.sh [base-port]
set -u

BASE_PORT=${1:-18090}
HOST=127.0.0.1
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERVER="$ROOT/server"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/shutdown-test.XXXXXX")
FAIL=0
SRV_PID=

cleanup() {
    if [ -n "$SRV_PID" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $1"
    FAIL=1
}

pass() {
    echo "PASS: $1"
}

start_server() {
    port=$1
    shift
    "$SERVER" "$port" "$ROOT/www" "$ROOT/cgi-bin" "$@" >"$TMP/server-$port.log" 2>&1 &
    SRV_PID=$!

    i=0
    while [ "$i" -lt 50 ]; do
        if nc -z "$HOST" "$port" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

# wait_exit PID TIMEOUT_SECONDS
# Returns the process's exit status, or 137 if the watchdog had to kill it.
wait_exit() {
    pid=$1
    timeout=$2

    ( trap - EXIT INT TERM; exec 3>&-; sleep "$timeout"; kill -9 "$pid" 2>/dev/null ) &
    watcher=$!

    wait "$pid" 2>/dev/null
    rc=$?

    kill -9 "$watcher" 2>/dev/null || true
    wait "$watcher" 2>/dev/null || true

    return "$rc"
}

# 1. idle SIGINT
if start_server "$BASE_PORT"; then
    kill -INT "$SRV_PID"
    wait_exit "$SRV_PID" 2
    rc=$?
    SRV_PID=
    if [ "$rc" -eq 0 ]; then
        pass "idle SIGINT exits cleanly"
    else
        fail "idle SIGINT (rc=$rc)"
    fi
else
    fail "idle SIGINT: server did not start"
    SRV_PID=
fi

# 2. idle SIGTERM
PORT2=$((BASE_PORT + 1))
if start_server "$PORT2"; then
    kill -TERM "$SRV_PID"
    wait_exit "$SRV_PID" 2
    rc=$?
    SRV_PID=
    if [ "$rc" -eq 0 ]; then
        pass "idle SIGTERM exits cleanly"
    else
        fail "idle SIGTERM (rc=$rc)"
    fi
else
    fail "idle SIGTERM: server did not start"
    SRV_PID=
fi

# 3. keep-alive: SIGINT after the first response, then a second request
PORT3=$((BASE_PORT + 2))
FIFO="$TMP/keepalive.fifo"
OUT="$TMP/keepalive.out"
if start_server "$PORT3"; then
    mkfifo "$FIFO"
    nc "$HOST" "$PORT3" <"$FIFO" >"$OUT" 2>/dev/null &
    NC_PID=$!
    exec 3>"$FIFO"

    /usr/bin/printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' >&3 2>/dev/null || true

    i=0
    while [ "$i" -lt 30 ]; do
        if grep -q 'HTTP/1.1 200' "$OUT" 2>/dev/null; then
            break
        fi
        sleep 0.1
        i=$((i + 1))
    done

    kill -INT "$SRV_PID"
    sleep 0.2
    /usr/bin/printf 'GET /missing HTTP/1.1\r\nHost: x\r\n\r\n' >&3 2>/dev/null || true
    exec 3>&-

    wait_exit "$NC_PID" 2
    wait_exit "$SRV_PID" 2
    rc=$?
    SRV_PID=

    first=$(grep -c 'HTTP/1.1 200' "$OUT" 2>/dev/null || true)
    total=$(grep -c '^HTTP/1.1' "$OUT" 2>/dev/null || true)

    if [ "$first" = "1" ] && [ "$total" = "1" ] && [ "$rc" -eq 0 ]; then
        pass "keep-alive SIGINT: first response only, then exit"
    else
        fail "keep-alive SIGINT (200=$first total=$total rc=$rc)"
    fi
else
    fail "keep-alive SIGINT: server did not start"
    SRV_PID=
fi

# 4. second SIGINT forces exit while CGI poll is waiting
PORT4=$((BASE_PORT + 3))
if start_server "$PORT4" --timeout 30; then
    curl -sS --max-time 10 "http://$HOST:$PORT4/cgi-bin/sleep" >/dev/null 2>&1 &
    CURL_PID=$!
    sleep 0.3

    kill -INT "$SRV_PID"
    sleep 0.1
    kill -INT "$SRV_PID"

    wait_exit "$SRV_PID" 1.5
    rc=$?
    SRV_PID=

    kill "$CURL_PID" 2>/dev/null || true
    wait "$CURL_PID" 2>/dev/null || true

    if [ "$rc" -eq 1 ]; then
        pass "second SIGINT forces exit"
    else
        fail "second SIGINT (rc=$rc, expected 1)"
    fi
else
    fail "second SIGINT: server did not start"
    SRV_PID=
fi

exit "$FAIL"
