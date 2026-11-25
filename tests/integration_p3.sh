#!/bin/sh
set -eu

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

TMP="${TMPDIR:-/tmp}/web_server_p3.$$"
mkdir -p "$TMP"
CONF="$TMP/p3.conf"
LOG="$TMP/proxy.log"
B1LOG="$TMP/b1.log"
B2LOG="$TMP/b2.log"

cat >"$CONF" <<EOF
listen $PPORT
docroot ./www
timeout connect 1
timeout response 2
timeout body 1
pool max_idle 4
pool idle_timeout 5

[route /api/]
type proxy
backend 127.0.0.1:$B1PORT
backend 127.0.0.1:$B2PORT

[route /]
type static
EOF

./tests/backend "$B1PORT" 1 >"$B1LOG" 2>&1 &
BPID1=$!
./tests/backend "$B2PORT" 2 >"$B2LOG" 2>&1 &
BPID2=$!

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
    cat "$B1LOG" "$B2LOG"
    exit 1
fi

./server --config "$CONF" >"$LOG" 2>&1 &
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
    if curl -sS --max-time 1 -o /dev/null "http://127.0.0.1:$PPORT/" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
if [ "$i" -ge 50 ]; then
    echo "FAIL: proxy did not start"
    cat "$LOG"
    exit 1
fi

fail=0
BASEURL="http://127.0.0.1:$PPORT"

check_status() {
    desc="$1"
    url="$2"
    expected="$3"
    code=$(curl -sS --max-time 5 -o /dev/null -w '%{http_code}' "$url" 2>/dev/null || echo "000")
    if [ "$code" = "$expected" ]; then
        echo "PASS: $desc ($code)"
    else
        echo "FAIL: $desc (expected $expected, got $code)"
        fail=1
    fi
}

check_body_contains() {
    desc="$1"
    url="$2"
    needle="$3"
    body=$(curl -sS --max-time 5 "$url" 2>/dev/null || true)
    case "$body" in
        *"$needle"*)
            echo "PASS: $desc"
            ;;
        *)
            echo "FAIL: $desc (missing '$needle')"
            fail=1
            ;;
    esac
}

check_status "static /" "$BASEURL/" "200"

# Round-Robin
ids=""
for i in 1 2 3 4 5 6; do
    body=$(curl -sS --max-time 5 "$BASEURL/api/id" 2>/dev/null || true)
    ids="$ids $(printf '%s' "$body" | tr -d '\n')"
done
case "$ids" in
    *id=1*id=2*|*id=2*id=1*)
        echo "PASS: Round-Robin hits both backends"
        ;;
    *)
        echo "FAIL: Round-Robin ids:$ids"
        fail=1
        ;;
esac

check_body_contains "proxy GET body" "$BASEURL/api/echo?x=1" "QUERY=x=1"
check_body_contains "X-Forwarded-For" "$BASEURL/api/echo" "XFF=127.0.0.1"

post_out=$(curl -sS --max-time 5 -d 'hello=world' "$BASEURL/api/echo" 2>/dev/null || true)
case "$post_out" in
    *"BODY=hello=world"*)
        echo "PASS: proxy POST body"
        ;;
    *)
        echo "FAIL: proxy POST body"
        fail=1
        ;;
esac

expect_out=$(curl -sS --max-time 5 -H 'Expect: 100-continue' \
    -d 'hello=world' "$BASEURL/api/echo" 2>/dev/null || true)
case "$expect_out" in
    *"BODY=hello=world"*)
        echo "PASS: Expect 100-continue"
        ;;
    *)
        echo "FAIL: Expect 100-continue"
        fail=1
        ;;
esac

hop_out=$(curl -sS --max-time 5 \
    -H 'Connection: X-Hop' -H 'X-Hop: secret' -H 'Keep-Alive: timeout=5' \
    "$BASEURL/api/echo" 2>/dev/null || true)
if printf '%s\n' "$hop_out" | grep -q '^CONN=keep-alive$' &&
   printf '%s\n' "$hop_out" | grep -q '^KEEPALIVE=$' &&
   printf '%s\n' "$hop_out" | grep -q '^XHOP=$'; then
    echo "PASS: hop-by-hop headers removed"
else
    echo "FAIL: hop-by-hop headers leaked"
    fail=1
fi

chunk_out=$(curl -sS --max-time 5 "$BASEURL/api/chunked" 2>/dev/null || true)
case "$chunk_out" in
    *"hello world!"*)
        echo "PASS: chunked response relay"
        ;;
    *)
        echo "FAIL: chunked response relay"
        fail=1
        ;;
esac

chunk_req=$({ printf 'POST /api/echo HTTP/1.1\r\nHost: x\r\n'
    printf 'Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n'
    printf '5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n'; } \
    | nc -w 3 127.0.0.1 "$PPORT" 2>/dev/null || true)
case "$chunk_req" in
    *"BODY=hello world"*)
        echo "PASS: chunked request relay"
        ;;
    *)
        echo "FAIL: chunked request relay"
        fail=1
        ;;
esac

# Upstream closes the connection after the response.
check_status "upstream Connection: close" "$BASEURL/api/close-cl" "200"
check_body_contains "close+CL body" "$BASEURL/api/close-cl" "closed-body"
check_status "close-delimited response" "$BASEURL/api/close-delimited" "200"
check_body_contains "close-delimited body" "$BASEURL/api/close-delimited" "close-delimited"
check_status "chunked response + close" "$BASEURL/api/close-chunked" "200"
check_body_contains "chunked close body" "$BASEURL/api/close-chunked" "hello"

# 1xx and final response in the same buffer.
check_status "1xx + 200 same buffer" "$BASEURL/api/interim" "200"
check_body_contains "1xx final body" "$BASEURL/api/interim" "ok"

# Response-side Connection-nominated header must not leak.
hopresp_headers=$(curl -sS --max-time 5 -D - -o /dev/null \
    "$BASEURL/api/hopresp" 2>/dev/null || true)
if printf '%s\n' "$hopresp_headers" | tr -d '\r' | grep -qi '^X-Hop:'; then
    echo "FAIL: response Connection-nominated header leaked"
    fail=1
else
    echo "PASS: response Connection-nominated header removed"
fi

# HTTP/1.0 upstream without keep-alive must not be pooled.
check_status "HTTP/1.0 upstream" "$BASEURL/api/http10" "200"
check_body_contains "HTTP/1.0 body" "$BASEURL/api/http10" "ok"

# 205 has no body.
check_status "205 Reset Content" "$BASEURL/api/reset205" "205"
body205=$(curl -sS --max-time 5 -o /dev/null -w '%{size_download}' \
    "$BASEURL/api/reset205" 2>/dev/null || echo "x")
if [ "$body205" = "0" ]; then
    echo "PASS: 205 no body"
else
    echo "FAIL: 205 body ($body205 bytes)"
    fail=1
fi

empty_code=$(curl -sS --max-time 5 -o /dev/null -w '%{http_code}' \
    "$BASEURL/api/empty200" 2>/dev/null || echo "000")
empty_size=$(curl -sS --max-time 5 -o /dev/null -w '%{size_download}' \
    "$BASEURL/api/empty200" 2>/dev/null || echo "x")
if [ "$empty_code" = "200" ] && [ "$empty_size" = "0" ]; then
    echo "PASS: 200 with Content-Length 0"
else
    echo "FAIL: 200 empty (code=$empty_code size=$empty_size)"
    fail=1
fi

# HEAD / 204 / 304 have no body.
head_code=$(curl -sS --max-time 5 -I -o /dev/null -w '%{http_code}' \
    "$BASEURL/api/head" 2>/dev/null || echo "000")
if [ "$head_code" = "200" ]; then
    echo "PASS: HEAD through proxy"
else
    echo "FAIL: HEAD through proxy (got $head_code)"
    fail=1
fi
for ep in no-content204 not-modified304; do
    code=$(curl -sS --max-time 5 -o /dev/null -w '%{http_code}' \
        "$BASEURL/api/$ep" 2>/dev/null || echo "000")
    size=$(curl -sS --max-time 5 -o /dev/null -w '%{size_download}' \
        "$BASEURL/api/$ep" 2>/dev/null || echo "x")
    if { [ "$ep" = "no-content204" ] && [ "$code" = "204" ] && [ "$size" = "0" ]; } ||
       { [ "$ep" = "not-modified304" ] && [ "$code" = "304" ] && [ "$size" = "0" ]; }; then
        echo "PASS: $ep no body"
    else
        echo "FAIL: $ep (code=$code size=$size)"
        fail=1
    fi
done

# Body idle timeout: 12 bytes over 4.8s with 1s idle limit.
drip_body=$(curl -sS --max-time 8 "$BASEURL/api/drip" 2>/dev/null || true)
if [ "$(printf '%s' "$drip_body" | wc -c | tr -d ' ')" = "12" ]; then
    echo "PASS: body idle timeout keeps slow response alive"
else
    echo "FAIL: body idle timeout cut slow response"
    fail=1
fi

# Prefix boundary: /api/ must not match /apix.
check_status "route prefix boundary /apix" "$BASEURL/apix" "404"

gzip_body=$(curl -sS --max-time 5 --compressed "$BASEURL/api/gzip" 2>/dev/null || true)
case "$gzip_body" in
    "hello gzip")
        echo "PASS: gzip pass-through"
        ;;
    *)
        echo "FAIL: gzip pass-through"
        fail=1
        ;;
esac

big_len=$(curl -sS --max-time 10 "$BASEURL/api/big" 2>/dev/null | wc -c | tr -d ' ')
if [ "$big_len" = "1048576" ]; then
    echo "PASS: 1MB response relay"
else
    echo "FAIL: 1MB response relay (got $big_len)"
    fail=1
fi

ka_count=$({ printf 'GET /api/id HTTP/1.1\r\nHost: x\r\n\r\n'; \
    sleep 0.4; \
    printf 'GET /api/id HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; } \
    | nc -w 3 127.0.0.1 "$PPORT" 2>/dev/null | grep -c 'HTTP/1.1 200' || true)
if [ "$ka_count" = "2" ]; then
    echo "PASS: proxy client keep-alive"
else
    echo "FAIL: proxy client keep-alive (got $ka_count)"
    fail=1
fi

# Client abort stress: exercise deferred proxy free without killing the server.
abort_pids=""
for i in $(seq 1 20); do
    curl -sS --max-time 0.05 -o /dev/null \
        "$BASEURL/api/slow?ms=1000" 2>/dev/null &
    abort_pids="$abort_pids $!"
done
for p in $abort_pids; do
    wait "$p" 2>/dev/null || true
done
sleep 0.3
check_status "server alive after client aborts" "$BASEURL/" "200"

b1stats=$(curl -sS --max-time 3 "http://127.0.0.1:$B1PORT/stats" 2>/dev/null || true)
b2stats=$(curl -sS --max-time 3 "http://127.0.0.1:$B2PORT/stats" 2>/dev/null || true)
b1conn=$(printf '%s' "$b1stats" | sed -n 's/.*connections=\([0-9][0-9]*\).*/\1/p')
b1req=$(printf '%s' "$b1stats" | sed -n 's/.*requests=\([0-9][0-9]*\).*/\1/p')
b2conn=$(printf '%s' "$b2stats" | sed -n 's/.*connections=\([0-9][0-9]*\).*/\1/p')
b2req=$(printf '%s' "$b2stats" | sed -n 's/.*requests=\([0-9][0-9]*\).*/\1/p')
if [ -n "$b1conn" ] && [ -n "$b1req" ] && [ "$b1conn" -lt "$b1req" ] &&
   [ -n "$b2conn" ] && [ -n "$b2req" ] && [ "$b2conn" -lt "$b2req" ]; then
    echo "PASS: pool reuse (b1 $b1conn/$b1req, b2 $b2conn/$b2req)"
else
    echo "FAIL: pool reuse (b1 $b1conn/$b1req, b2 $b2conn/$b2req)"
    fail=1
fi

# Failover: one backend down, the other must serve the request.
kill -TERM "$BPID1" 2>/dev/null || true
wait "$BPID1" 2>/dev/null || true
sleep 0.2
failover_out=$(curl -sS --max-time 4 -o /dev/null -w '%{http_code}' \
    "$BASEURL/api/id" 2>/dev/null || echo "000")
if [ "$failover_out" = "200" ]; then
    echo "PASS: failover to healthy backend"
else
    echo "FAIL: failover to healthy backend (got $failover_out)"
    fail=1
fi

# 504: backend is slow, proxy response timeout is 2s.
check_status "backend timeout -> 504" "$BASEURL/api/slow?ms=3000" "504"

# 502: stop the remaining backend.
kill -TERM "$BPID2" 2>/dev/null || true
wait "$BPID2" 2>/dev/null || true
sleep 0.2
check_status "backend down -> 502" "$BASEURL/api/id" "502"

# config fail-fast: proxy route without backend
BADCONF="$TMP/bad.conf"
cat >"$BADCONF" <<EOF
listen $((BASE + 3))
docroot ./www
[route /bad/]
type proxy
[route /]
type static
EOF
if ./server --config "$BADCONF" >"$TMP/bad.log" 2>&1; then
    echo "FAIL: invalid config accepted"
    fail=1
else
    echo "PASS: invalid config rejected"
fi

if [ "$fail" -ne 0 ]; then
    echo "--- proxy log ---"
    cat "$LOG"
    echo "--- backend 1 log ---"
    cat "$B1LOG"
    echo "--- backend 2 log ---"
    cat "$B2LOG"
fi

exit "$fail"
