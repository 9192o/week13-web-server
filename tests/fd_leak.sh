#!/bin/sh
set -eu

PORT="${PORT:-18081}"
LOG="${TMPDIR:-/tmp}/web_server_fd.$$.log"

./server "$PORT" ./www ./cgi-bin >"$LOG" 2>&1 &
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

fd_count() {
    if [ -d "/proc/$pid/fd" ]; then
        ls "/proc/$pid/fd" | wc -l | tr -d ' '
    else
        lsof -p "$pid" -a -d 0-999 -F fd 2>/dev/null \
            | grep -c '^f' || true
    fi
}

before=$(fd_count)

if command -v ab >/dev/null 2>&1; then
    ab -q -n 300 -c 1 "http://127.0.0.1:$PORT/index.html" >/dev/null 2>&1 || true
    ab -q -k -n 300 -c 1 "http://127.0.0.1:$PORT/index.html" >/dev/null 2>&1 || true
else
    i=0
    while [ "$i" -lt 200 ]; do
        curl -sS --max-time 2 -o /dev/null \
            "http://127.0.0.1:$PORT/index.html" >/dev/null 2>&1 || true
        i=$((i + 1))
    done
fi

i=0
while [ "$i" -lt 50 ]; do
    curl -sS --max-time 2 -o /dev/null \
        "http://127.0.0.1:$PORT/cgi-bin/echo?x=1" >/dev/null 2>&1 || true
    i=$((i + 1))
done

sleep 0.3
after=$(fd_count)

echo "fd before=$before after=$after"
if [ "$after" -le $((before + 2)) ]; then
    echo "PASS: fd leak check"
    exit 0
fi

echo "FAIL: fd leak check"
cat "$LOG"
exit 1
