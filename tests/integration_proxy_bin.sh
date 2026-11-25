#!/bin/sh
set -eu

find_base_ports() {
    for p in 19090 19190 19290 19390; do
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

TMP="${TMPDIR:-/tmp}/web_server_proxy_bin.$$"
mkdir -p "$TMP"

cat >"$TMP/proxy.conf" <<EOF
listen $PPORT
timeout request 5
timeout connect 1
timeout response 2
timeout body 2
pool max_idle 4
pool idle_timeout 2

[route /api/]
type proxy
backend 127.0.0.1:$B1PORT
backend 127.0.0.1:$B2PORT

[route /strip/]
type proxy
strip_prefix 1
backend 127.0.0.1:$B1PORT
backend 127.0.0.1:$B2PORT
EOF

fail=0

./tests/backend "$B1PORT" 1 >"$TMP/b1.log" 2>&1 &
BPID1=$!
./tests/backend "$B2PORT" 2 >"$TMP/b2.log" 2>&1 &
BPID2=$!
./reverse-proxy --config "$TMP/proxy.conf" >"$TMP/proxy.log" 2>&1 &
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
    if curl -sS --max-time 1 -o /dev/null \
        "http://127.0.0.1:$PPORT/api/id" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
if [ "$i" -ge 50 ]; then
    echo "FAIL: reverse-proxy did not start"
    cat "$TMP/proxy.log"
    exit 1
fi

BASEURL="http://127.0.0.1:$PPORT"

check_status() {
    desc="$1"
    url="$2"
    want="$3"
    got=$(curl -sS --max-time 5 -o /dev/null -w '%{http_code}' "$url" 2>/dev/null || echo "000")
    if [ "$got" = "$want" ]; then
        echo "PASS: $desc"
    else
        echo "FAIL: $desc (want $want, got $got)"
        fail=1
    fi
}

check_body() {
    desc="$1"
    url="$2"
    needle="$3"
    body=$(curl -sS --max-time 5 "$url" 2>/dev/null || true)
    case "$body" in
        *"$needle"*) echo "PASS: $desc" ;;
        *)
            echo "FAIL: $desc (missing '$needle')"
            fail=1
            ;;
    esac
}

check_status "GET /api/id" "$BASEURL/api/id" "200"
check_body "round-robin body" "$BASEURL/api/id" "id="
check_status "unmatched path -> 404" "$BASEURL/nope" "404"
check_status "root -> 404" "$BASEURL/" "404"
check_body "strip_prefix path" "$BASEURL/strip/echo" "PATH=/echo"
check_body "strip_prefix query" "$BASEURL/strip/echo?x=1" "QUERY=x=1"
check_body "chunked response" "$BASEURL/api/chunked" "hello world"

post_out=$(curl -sS --max-time 5 -d 'hello=world' \
    "$BASEURL/api/echo" 2>/dev/null || true)
case "$post_out" in
    *"BODY=hello=world"*) echo "PASS: POST body" ;;
    *)
        echo "FAIL: POST body (missing echoed body)"
        fail=1
        ;;
esac

expect_out=$(curl -sS --max-time 5 -H 'Expect: 100-continue' \
    -d 'hello=world' "$BASEURL/api/echo" 2>/dev/null || true)
case "$expect_out" in
    *"BODY=hello=world"*) echo "PASS: Expect 100-continue" ;;
    *)
        echo "FAIL: Expect 100-continue"
        fail=1
        ;;
esac

ka_out=$({ printf 'GET /api/id HTTP/1.1\r\nHost: x\r\n\r\n'; \
    sleep 0.3; \
    printf 'GET /api/id HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; } \
    | nc -w 3 127.0.0.1 "$PPORT" 2>/dev/null || true)
ka_count=$(printf '%s' "$ka_out" | grep -c 'HTTP/1.1 200' || true)
if [ "$ka_count" = "2" ]; then
    echo "PASS: proxy client keep-alive"
else
    echo "FAIL: proxy client keep-alive (got $ka_count)"
    fail=1
fi

check_status "slow backend -> 504" "$BASEURL/api/slow?ms=3000" "504"

kill -TERM "$BPID1" "$BPID2" 2>/dev/null || true
wait "$BPID1" 2>/dev/null || true
wait "$BPID2" 2>/dev/null || true
sleep 0.2
check_status "backends down -> 502" "$BASEURL/api/id" "502"

echo "--- reverse-proxy log ---"
cat "$TMP/proxy.log"

exit "$fail"
