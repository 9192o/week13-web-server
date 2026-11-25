#!/bin/sh
set -eu

find_base_ports() {
    for p in 19490 19590 19690 19790; do
        if ! nc -z 127.0.0.1 "$p" >/dev/null 2>&1 &&
           ! nc -z 127.0.0.1 "$((p + 1))" >/dev/null 2>&1 &&
           ! nc -z 127.0.0.1 "$((p + 2))" >/dev/null 2>&1 &&
           ! nc -z 127.0.0.1 "$((p + 3))" >/dev/null 2>&1; then
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
BPORT=$((BASE + 1))
TPORT=$((BASE + 2))
LPORT=$((BASE + 3))

TMP="${TMPDIR:-/tmp}/web_server_proxy_front.$$"
mkdir -p "$TMP"

cat >"$TMP/backend.conf" <<EOF
listen $BPORT
docroot ./www
cgi_dir ./cgi-bin
timeout request 5
keepalive_idle 5
keepalive_max 100
max_conns 64
task_workers 2
task_queue 16

[route /]
type static
EOF

cat >"$TMP/proxy.conf" <<EOF
listen $PPORT
timeout request 5
timeout connect 1
timeout response 3
timeout body 2
max_conns 64
max_upstreams 32

[route /api/]
type proxy
backend 127.0.0.1:$TPORT

[route /]
type proxy
backend 127.0.0.1:$BPORT
EOF

cat >"$TMP/limit.conf" <<EOF
listen $LPORT
timeout request 5
timeout connect 1
timeout response 3
timeout body 2
max_conns 2
max_upstreams 2

[route /api/]
type proxy
backend 127.0.0.1:$TPORT
EOF

fail=0

./tests/backend "$TPORT" 7 >"$TMP/testbackend.log" 2>&1 &
TPID=$!
i=0
while [ "$i" -lt 50 ]; do
    if curl -sS --max-time 1 -o /dev/null "http://127.0.0.1:$TPORT/health" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
./backend-server --config "$TMP/backend.conf" >"$TMP/backend.log" 2>&1 &
BPID=$!
./reverse-proxy --config "$TMP/proxy.conf" >"$TMP/proxy.log" 2>&1 &
PROXYPID=$!
LPID=""

cleanup() {
    kill -INT "$PROXYPID" 2>/dev/null || true
    [ -z "$LPID" ] || kill -INT "$LPID" 2>/dev/null || true
    kill -INT "$BPID" 2>/dev/null || true
    kill -INT "$TPID" 2>/dev/null || true
    wait "$PROXYPID" 2>/dev/null || true
    [ -z "$LPID" ] || wait "$LPID" 2>/dev/null || true
    wait "$BPID" 2>/dev/null || true
    wait "$TPID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

i=0
while [ "$i" -lt 50 ]; do
    if curl -fsS --max-time 1 -o /dev/null "http://127.0.0.1:$PPORT/" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
if [ "$i" -ge 50 ]; then
    echo "FAIL: proxy-first stack did not start"
    cat "$TMP/backend.log" "$TMP/proxy.log"
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

check_status "client -> proxy -> backend /" "$BASEURL/" "200"
check_body "proxy-first static body" "$BASEURL/" "<!DOCTYPE html>"
check_status "proxy-first CGI" "$BASEURL/cgi-bin/echo?x=1" "200"
check_body "proxy-first CGI query" "$BASEURL/cgi-bin/echo?x=1" "x=1"
check_status "proxy-first 404 from backend" "$BASEURL/missing" "404"

check_status "front -> test backend /api/id" "$BASEURL/api/id" "200"
check_body "front -> test backend body" "$BASEURL/api/id" "id=7"
check_body "front chunked relay" "$BASEURL/api/chunked" "hello world"

hop_headers=$(curl -sS --max-time 5 -D - -o /dev/null \
    "$BASEURL/api/hopresp" 2>/dev/null || true)
if printf '%s\n' "$hop_headers" | tr -d '\r' | grep -qi '^X-Hop:'; then
    echo "FAIL: front hop-by-hop header leaked"
    fail=1
else
    echo "PASS: front hop-by-hop header removed"
fi

trailer_out=$({ printf 'GET /api/trailer HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; } \
    | nc -w 3 127.0.0.1 "$PPORT" 2>/dev/null || true)
case "$trailer_out" in
    *"X-Sum: 42"*) echo "PASS: front chunked trailer" ;;
    *)
        echo "FAIL: front chunked trailer"
        fail=1
        ;;
esac

ka_out=$({ printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n'; sleep 0.3; \
    printf 'GET /api/id HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; } \
    | nc -w 3 127.0.0.1 "$PPORT" 2>/dev/null || true)
ka_count=$(printf '%s' "$ka_out" | grep -c 'HTTP/1.1 200' || true)
if [ "$ka_count" = "2" ]; then
    echo "PASS: front client keep-alive"
else
    echo "FAIL: front client keep-alive (got $ka_count)"
    fail=1
fi

# max_conns counts client + active upstream connections.
./reverse-proxy --config "$TMP/limit.conf" >"$TMP/limit.log" 2>&1 &
LPID=$!
i=0
while [ "$i" -lt 50 ]; do
    if curl -fsS --max-time 1 -o /dev/null "http://127.0.0.1:$LPORT/api/id" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
curl -sS --max-time 5 -o /dev/null "http://127.0.0.1:$LPORT/api/slow?ms=1000" 2>/dev/null &
slow_pid=$!
sleep 0.2
limit_code=$(curl -sS --max-time 3 -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$LPORT/api/id" 2>/dev/null || echo "000")
wait "$slow_pid" 2>/dev/null || true
if [ "$limit_code" = "503" ]; then
    echo "PASS: max_conns includes active upstream"
else
    echo "FAIL: max_conns includes active upstream (got $limit_code)"
    fail=1
fi
kill -INT "$LPID" 2>/dev/null || true
wait "$LPID" 2>/dev/null || true
LPID=""

kill -TERM "$BPID" 2>/dev/null || true
wait "$BPID" 2>/dev/null || true
sleep 0.2
check_status "backend down through proxy" "$BASEURL/" "502"
check_status "test backend still alive" "$BASEURL/api/id" "200"

kill -TERM "$TPID" 2>/dev/null || true
wait "$TPID" 2>/dev/null || true
sleep 0.2
check_status "test backend down through proxy" "$BASEURL/api/id" "502"

echo "--- backend log ---"
cat "$TMP/backend.log"
echo "--- test backend log ---"
cat "$TMP/testbackend.log"
echo "--- proxy log ---"
cat "$TMP/proxy.log"

exit "$fail"
