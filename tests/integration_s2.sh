#!/bin/sh
set -eu

pick_free_port() {
    for p in 18090 18190 18290 18390 18490 18590; do
        if ! nc -z 127.0.0.1 "$p" >/dev/null 2>&1; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

LOG="${TMPDIR:-/tmp}/web_server_s2.$$.log"
TESTROOT="${TMPDIR:-/tmp}/web_server_s2_docroot.$$"
mkdir -p "$TESTROOT"
cp www/index.html "$TESTROOT/index.html"

pid=""
aux_pids=""
cleanup() {
    for p in $aux_pids; do
        kill "$p" 2>/dev/null || true
    done
    [ -z "${pid:-}" ] || kill -INT "$pid" 2>/dev/null || true
    [ -z "${pid:-}" ] || wait "$pid" 2>/dev/null || true
    rm -rf "$TESTROOT"
}
trap cleanup EXIT INT TERM

PORT=$(pick_free_port) || {
    echo "error: no free port found" >&2
    exit 1
}
BASE="http://127.0.0.1:$PORT"

./server_ev "$PORT" "$TESTROOT" ./cgi-bin \
    --max-conns 2 --task-workers 1 --task-queue 1 --timeout 5 \
    >"$LOG" 2>&1 &
pid=$!

i=0
while [ "$i" -lt 50 ]; do
    if curl -sS --max-time 1 -o /dev/null "$BASE/" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
if [ "$i" -ge 50 ]; then
    echo "FAIL: server did not start"
    cat "$LOG"
    exit 1
fi

fail=0

# max-conns: two idle connections fill the limit; the third gets 503.
{ sleep 5; } | nc 127.0.0.1 "$PORT" >/dev/null 2>&1 &
c1=$!
{ sleep 5; } | nc 127.0.0.1 "$PORT" >/dev/null 2>&1 &
c2=$!
aux_pids="$c1 $c2"
sleep 0.5

headers=$(curl -sS --max-time 2 -D - -o /dev/null "$BASE/index.html" 2>/dev/null || true)
code=$(printf '%s' "$headers" | awk 'NR==1{print $2}')
case "$headers" in
    *"Retry-After: 1"*) retry=1 ;;
    *) retry=0 ;;
esac
if [ "$code" = "503" ] && [ "$retry" = "1" ]; then
    echo "PASS: max-conns overflow -> 503 + Retry-After"
else
    echo "FAIL: max-conns overflow (code=$code retry=$retry)"
    fail=1
fi

# Closing one idle connection frees a slot.
kill "$c1" 2>/dev/null || true
wait "$c1" 2>/dev/null || true
i=0
while [ "$i" -lt 30 ]; do
    code=$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' \
        "$BASE/index.html" 2>/dev/null || echo "000")
    [ "$code" = "200" ] && break
    i=$((i + 1))
    sleep 0.1
done
if [ "$code" = "200" ]; then
    echo "PASS: max-conns slot recovery -> 200"
else
    echo "FAIL: max-conns slot recovery (code=$code)"
    fail=1
fi
kill "$c2" 2>/dev/null || true
wait "$c2" 2>/dev/null || true
aux_pids=""
sleep 0.5

# task queue: one worker + one queued job means the third CGI gets 503.
qpids=""
for n in 1 2 3; do
    curl -sS --max-time 10 -D "$TESTROOT/q$n.hdr" -o /dev/null \
        -w '%{http_code}' "$BASE/cgi-bin/sleep" > "$TESTROOT/q$n.code" \
        2>/dev/null &
    qpids="$qpids $!"
done
for p in $qpids; do
    wait "$p" 2>/dev/null || true
done

ok=0
busy=0
bad=0
for n in 1 2 3; do
    code=$(cat "$TESTROOT/q$n.code" 2>/dev/null || echo "000")
    case "$code" in
        200) ok=$((ok + 1)) ;;
        503) busy=$((busy + 1)) ;;
        *) bad=$((bad + 1)) ;;
    esac
done
if [ "$bad" = "0" ] && [ "$busy" -ge 1 ] && [ "$ok" -ge 1 ]; then
    echo "PASS: task queue full -> 503 ($ok ok, $busy busy)"
else
    echo "FAIL: task queue full (ok=$ok busy=$busy bad=$bad)"
    fail=1
fi

retry=0
for n in 1 2 3; do
    [ "$(cat "$TESTROOT/q$n.code" 2>/dev/null || echo 000)" = "503" ] || continue
    if grep -q '^Retry-After: 1' "$TESTROOT/q$n.hdr" 2>/dev/null; then
        retry=1
    fi
done
if [ "$retry" = "1" ]; then
    echo "PASS: queue-full Retry-After"
else
    echo "FAIL: queue-full Retry-After missing"
    fail=1
fi

code=$(curl -sS --max-time 3 -o /dev/null -w '%{http_code}' \
    "$BASE/index.html" 2>/dev/null || echo "000")
if [ "$code" = "200" ]; then
    echo "PASS: server alive after backpressure"
else
    echo "FAIL: server alive after backpressure (code=$code)"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- server_ev log ---"
    cat "$LOG"
fi

exit "$fail"
