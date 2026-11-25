#!/bin/sh
set -eu

find_base_ports() {
    for p in 19490 19590 19690 19790; do
        if ! nc -z 127.0.0.1 "$p" >/dev/null 2>&1 &&
           ! nc -z 127.0.0.1 "$((p + 1))" >/dev/null 2>&1; then
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

[route /]
type proxy
backend 127.0.0.1:$BPORT
EOF

fail=0

./server --backend --config "$TMP/backend.conf" >"$TMP/backend.log" 2>&1 &
BPID=$!
./reverse-proxy --config "$TMP/proxy.conf" >"$TMP/proxy.log" 2>&1 &
PROXYPID=$!

cleanup() {
    kill -INT "$PROXYPID" 2>/dev/null || true
    kill -INT "$BPID" 2>/dev/null || true
    wait "$PROXYPID" 2>/dev/null || true
    wait "$BPID" 2>/dev/null || true
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

kill -TERM "$BPID" 2>/dev/null || true
wait "$BPID" 2>/dev/null || true
sleep 0.2
check_status "backend down through proxy" "$BASEURL/" "502"

echo "--- backend log ---"
cat "$TMP/backend.log"
echo "--- proxy log ---"
cat "$TMP/proxy.log"

exit "$fail"
