#!/bin/sh
set -eu

if ! command -v ab >/dev/null 2>&1; then
    echo "SKIP: ab not found"
    exit 0
fi

find_base_ports() {
    for p in 18090 18190 18290 18390 18490 18590; do
        if ! nc -z 127.0.0.1 "$p" >/dev/null 2>&1 &&
           ! nc -z 127.0.0.1 "$((p + 1))" >/dev/null 2>&1 &&
           ! nc -z 127.0.0.1 "$((p + 2))" >/dev/null 2>&1; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

BASE=$(find_base_ports) || {
    echo "error: no free ports" >&2
    exit 1
}
PPORT=$BASE
B1PORT=$((BASE + 1))
B2PORT=$((BASE + 2))
TMP="${TMPDIR:-/tmp}/web_server_proxy_load.$$"
mkdir -p "$TMP"

cat >"$TMP/load.conf" <<EOF
listen $PPORT
docroot ./www
timeout connect 2
timeout response 10
timeout body 10
pool max_idle 8
pool idle_timeout 30
[route /api/]
type proxy
backend 127.0.0.1:$B1PORT
backend 127.0.0.1:$B2PORT
[route /]
type static
EOF

./tests/backend "$B1PORT" 1 >"$TMP/b1.log" 2>&1 &
BPID1=$!
./tests/backend "$B2PORT" 2 >"$TMP/b2.log" 2>&1 &
BPID2=$!
./server --config "$TMP/load.conf" >"$TMP/proxy.log" 2>&1 &
PID=$!

cleanup() {
    kill -INT "$PID" 2>/dev/null || true
    kill -TERM "$BPID1" 2>/dev/null || true
    kill -TERM "$BPID2" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
    wait "$BPID1" 2>/dev/null || true
    wait "$BPID2" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

i=0
while [ "$i" -lt 50 ]; do
    if curl -sS --max-time 1 -o /dev/null \
        "http://127.0.0.1:$B1PORT/health" 2>/dev/null &&
       curl -sS --max-time 1 -o /dev/null \
        "http://127.0.0.1:$B2PORT/health" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
if [ "$i" -ge 50 ]; then
    echo "FAIL: backends did not start"
    cat "$TMP/b1.log" "$TMP/b2.log"
    exit 1
fi

i=0
while [ "$i" -lt 50 ]; do
    if curl -sS --max-time 1 -o /dev/null "http://127.0.0.1:$PPORT/" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
if [ "$i" -ge 50 ]; then
    echo "FAIL: proxy did not start"
    cat "$TMP/proxy.log"
    exit 1
fi

out="$TMP/ab.out"
ab -q -k -n 10000 -c 100 "http://127.0.0.1:$PPORT/api/id" >"$out" 2>&1 || {
    echo "FAIL: ab failed"
    cat "$out"
    exit 1
}

failed=$(awk '/Failed requests:/{print $3}' "$out")
non2xx=$(awk '/Non-2xx responses:/{print $3}' "$out")
rps=$(awk '/Requests per second:/{print $4}' "$out")
echo "proxy load: failed=${failed:-0} non2xx=${non2xx:-0} rps=${rps:-?}"

if [ "${failed:-0}" != "0" ]; then
    echo "FAIL: failed requests"
    exit 1
fi
if [ -n "${non2xx:-}" ] && [ "$non2xx" != "0" ]; then
    echo "FAIL: non-2xx responses"
    exit 1
fi
