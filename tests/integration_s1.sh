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

process_alive() {
    state=$(ps -p "$1" -o stat= 2>/dev/null | tr -d ' ')
    case "$state" in
        ""|Z*) return 1 ;;
    esac
    return 0
}

if [ -z "${PORT:-}" ]; then
    PORT=$(pick_free_port) || {
        echo "error: no free port found" >&2
        exit 1
    }
fi

LOG="${TMPDIR:-/tmp}/web_server_s1.$$.log"
TESTROOT="${TMPDIR:-/tmp}/web_server_s1_docroot.$$"

mkdir -p "$TESTROOT/dir"
cp www/index.html "$TESTROOT/index.html"
printf '<html><body>dir index</body></html>\n' > "$TESTROOT/dir/index.html"

./server_ev "$PORT" "$TESTROOT" ./cgi-bin >"$LOG" 2>&1 &
pid=$!
tpid=""

cleanup() {
    [ -z "${pid:-}" ] || kill -INT "$pid" 2>/dev/null || true
    [ -z "${tpid:-}" ] || kill -INT "$tpid" 2>/dev/null || true
    [ -z "${pid:-}" ] || wait "$pid" 2>/dev/null || true
    [ -z "${tpid:-}" ] || wait "$tpid" 2>/dev/null || true
    rm -rf "$TESTROOT"
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
BASE="http://127.0.0.1:$PORT"

check_status() {
    desc="$1"
    url="$2"
    expected="$3"
    code=$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' "$url" 2>/dev/null || echo "000")
    if [ "$code" = "$expected" ]; then
        echo "PASS: $desc ($code)"
    else
        echo "FAIL: $desc (expected $expected, got $code)"
        fail=1
    fi
}

check_status_slow() {
    desc="$1"
    url="$2"
    expected="$3"
    code=$(curl -sS --max-time 6 -o /dev/null -w '%{http_code}' "$url" 2>/dev/null || echo "000")
    if [ "$code" = "$expected" ]; then
        echo "PASS: $desc ($code)"
    else
        echo "FAIL: $desc (expected $expected, got $code)"
        fail=1
    fi
}

wait_log_count() {
    pattern="$1"
    want="$2"
    i=0
    count=0
    while [ "$i" -lt 30 ]; do
        count=$(grep -c "$pattern" "$LOG" 2>/dev/null || true)
        [ "$count" -ge "$want" ] && break
        i=$((i + 1))
        sleep 0.1
    done
    echo "$count"
    [ "$count" -ge "$want" ]
}

check_body() {
    desc="$1"
    url="$2"
    needle="$3"
    body=$(curl -sS --max-time 2 "$url" 2>/dev/null || true)
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

check_header() {
    desc="$1"
    url="$2"
    needle="$3"
    headers=$(curl -sS --max-time 2 -D - -o /dev/null "$url" 2>/dev/null || true)
    case "$headers" in
        *"$needle"*)
            echo "PASS: $desc"
            ;;
        *)
            echo "FAIL: $desc (missing '$needle')"
            fail=1
            ;;
    esac
}

check_head_no_body() {
    desc="$1"
    path="$2"
    raw=$(printf 'HEAD %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' "$path" \
        | nc -w 3 127.0.0.1 "$PORT" 2>/dev/null || true)
    body=$(printf '%s' "$raw" | tr -d '\r' | awk 'BEGIN{b=0} /^$/{b=1;next} b{print}')
    if [ -z "$body" ]; then
        echo "PASS: $desc (no body)"
    else
        echo "FAIL: $desc (HEAD body leaked)"
        fail=1
    fi
}

check_status "GET /" "$BASE/" "200"
check_body "body /" "$BASE/" "It works!"
check_status "GET /missing" "$BASE/missing" "404"
check_status "GET /dir" "$BASE/dir" "301"
check_header "Location /dir/" "$BASE/dir" "Location: /dir/"
check_status "GET /dir/" "$BASE/dir/" "200"
check_body "body /dir/" "$BASE/dir/" "dir index"
check_head_no_body "HEAD /index.html" "/index.html"

range_headers=$(curl -sS --max-time 2 -H 'Range: bytes=0-4' -D - \
    -o "$TESTROOT/range.body" "$BASE/index.html" 2>/dev/null || true)
case "$range_headers" in
    *"HTTP/1.1 206"*"Content-Range: bytes 0-4/"*)
        echo "PASS: Range 0-4 -> 206"
        ;;
    *)
        echo "FAIL: Range 0-4 (expected 206 + Content-Range)"
        fail=1
        ;;
esac
range_len=$(wc -c < "$TESTROOT/range.body" 2>/dev/null | tr -d ' ' || echo 0)
if [ "$range_len" = "5" ]; then
    echo "PASS: Range body length 5"
else
    echo "FAIL: Range body length (got $range_len)"
    fail=1
fi
check_header "Accept-Ranges" "$BASE/index.html" "Accept-Ranges: bytes"

last_modified=$(curl -sS --max-time 2 -D - -o /dev/null "$BASE/index.html" 2>/dev/null \
    | tr -d '\r' | awk -F': ' 'tolower($1)=="last-modified"{print $2; exit}')
if [ -n "$last_modified" ]; then
    code=$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' \
        -H "If-Modified-Since: $last_modified" "$BASE/index.html" 2>/dev/null || echo "000")
    if [ "$code" = "304" ]; then
        echo "PASS: If-Modified-Since -> 304"
    else
        echo "FAIL: If-Modified-Since (expected 304, got $code)"
        fail=1
    fi
else
    echo "FAIL: Last-Modified missing"
    fail=1
fi

options_out=$(curl -sS -X OPTIONS --max-time 2 -D - -o /dev/null \
    "$BASE/index.html" 2>/dev/null || true)
case "$options_out" in
    *"HTTP/1.1 200"*"Allow: GET, POST, HEAD, OPTIONS"*)
        echo "PASS: OPTIONS + Allow"
        ;;
    *)
        echo "FAIL: OPTIONS + Allow"
        fail=1
        ;;
esac

ka_out=$({ printf 'GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n'; \
    printf 'GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; } \
    | nc -w 3 127.0.0.1 "$PORT" 2>/dev/null || true)
ka_count=$(printf '%s' "$ka_out" | grep -c 'HTTP/1.1 200 OK' || true)
if [ "$ka_count" = "2" ]; then
    echo "PASS: keep-alive 2 requests"
else
    echo "FAIL: keep-alive (expected 2 responses, got $ka_count)"
    fail=1
fi

# 20 concurrent requests: all must be 200 and bodies must not cross.
log_before=$(grep -c '^127.0.0.1 "GET /index.html" 200 ' "$LOG" 2>/dev/null || true)
pids=""
for i in $(seq 1 20); do
    curl -sS --max-time 5 -o "$TESTROOT/out.$i" -w '%{http_code}' \
        "$BASE/index.html" > "$TESTROOT/code.$i" 2>/dev/null &
    pids="$pids $!"
done
for p in $pids; do
    wait "$p" 2>/dev/null || true
done
bad=0
for i in $(seq 1 20); do
    [ "$(cat "$TESTROOT/code.$i" 2>/dev/null)" = "200" ] || bad=$((bad + 1))
    grep -q 'It works!' "$TESTROOT/out.$i" 2>/dev/null || bad=$((bad + 1))
done
if [ "$bad" = "0" ]; then
    echo "PASS: 20 concurrent requests"
else
    echo "FAIL: concurrent requests ($bad problems)"
    fail=1
fi

want=$((log_before + 20))
log_count=$(wait_log_count '^127.0.0.1 "GET /index.html" 200 ' "$want") || true
if [ "$log_count" -ge "$want" ]; then
    echo "PASS: access log 20 concurrent requests"
else
    echo "FAIL: access log concurrent ($log_count < $want)"
    fail=1
fi

# A slow header client must not block a normal request.
{ printf 'GET / HTTP/1.1\r\nHost: x\r\n'; sleep 2; printf '\r\n'; } \
    | nc -w 3 127.0.0.1 "$PORT" >/dev/null 2>&1 &
slow_pid=$!
sleep 0.2
result=$(curl -sS --max-time 2 -o /dev/null -w '%{http_code} %{time_total}' \
    "$BASE/index.html" 2>/dev/null || echo "000 9")
wait "$slow_pid" 2>/dev/null || true
code=$(printf '%s' "$result" | awk '{print $1}')
elapsed=$(printf '%s' "$result" | awk '{print $2}')
if [ "$code" = "200" ] && awk "BEGIN{exit !($elapsed < 1.0)}"; then
    echo "PASS: slow client does not block ($elapsed s)"
else
    echo "FAIL: slow client blocked normal request (code=$code, time=$elapsed)"
    fail=1
fi

check_status "CGI GET" "$BASE/cgi-bin/echo?x=1" "200"
check_body "CGI GET query" "$BASE/cgi-bin/echo?x=1" "QUERY=x=1"

post_out=$(curl -sS --max-time 5 -d 'hello=world' \
    "$BASE/cgi-bin/echo" 2>/dev/null || true)
case "$post_out" in
    *"BODY=hello=world"*)
        echo "PASS: CGI POST body"
        ;;
    *)
        echo "FAIL: CGI POST body"
        fail=1
        ;;
esac

check_status "CGI crash" "$BASE/cgi-bin/crash" "500"

notfound_log=$(wait_log_count '^127.0.0.1 "GET /missing" 404 ' 1) || true
if [ "$notfound_log" -ge 1 ]; then
    echo "PASS: access log 404"
else
    echo "FAIL: access log 404 missing"
    fail=1
fi

crash_log=$(wait_log_count '^127.0.0.1 "GET /cgi-bin/crash" 500 ' 1) || true
if [ "$crash_log" -ge 1 ]; then
    echo "PASS: access log CGI 500"
else
    echo "FAIL: access log CGI 500 missing"
    fail=1
fi

# Concurrent CGI: task pool workers must keep each query separate.
cgi_log_before=$(grep -c '^127.0.0.1 "GET /cgi-bin/echo" 200 ' "$LOG" 2>/dev/null || true)
cgipids=""
for i in $(seq 1 10); do
    curl -sS --max-time 5 -o "$TESTROOT/cgi.$i" \
        "$BASE/cgi-bin/echo?x=$i" 2>/dev/null &
    cgipids="$cgipids $!"
done
for p in $cgipids; do
    wait "$p" 2>/dev/null || true
done
cgibad=0
for i in $(seq 1 10); do
    grep -q "QUERY=x=$i" "$TESTROOT/cgi.$i" 2>/dev/null || cgibad=$((cgibad + 1))
done
if [ "$cgibad" = "0" ]; then
    echo "PASS: 10 concurrent CGI"
else
    echo "FAIL: concurrent CGI ($cgibad mismatches)"
    fail=1
fi

cgi_want=$((cgi_log_before + 10))
cgi_log_count=$(wait_log_count '^127.0.0.1 "GET /cgi-bin/echo" 200 ' "$cgi_want") || true
if [ "$cgi_log_count" -ge "$cgi_want" ]; then
    echo "PASS: access log 10 concurrent CGI"
else
    echo "FAIL: access log concurrent CGI ($cgi_log_count < $cgi_want)"
    fail=1
fi

cgi_ka_out=$({ printf 'GET /cgi-bin/echo?x=1 HTTP/1.1\r\nHost: x\r\n\r\n'; \
    printf 'GET /cgi-bin/echo?x=2 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; } \
    | nc -w 3 127.0.0.1 "$PORT" 2>/dev/null || true)
cgi_ka_count=$(printf '%s' "$cgi_ka_out" | grep -c 'HTTP/1.1 200 OK' || true)
if [ "$cgi_ka_count" = "2" ]; then
    echo "PASS: CGI keep-alive 2 requests"
else
    echo "FAIL: CGI keep-alive (expected 2 responses, got $cgi_ka_count)"
    fail=1
fi

# CGI timeout: a script that exceeds --timeout must get 504 and be killed,
# even when it ignores SIGTERM. The server must keep serving afterwards.
TPORT=$(pick_free_port) || {
    echo "FAIL: no free port for timeout server"
    fail=1
    TPORT=""
}
if [ -n "$TPORT" ]; then
    TBASE="http://127.0.0.1:$TPORT"
    ./server_ev "$TPORT" "$TESTROOT" ./cgi-bin --timeout 1 \
        >"$LOG.timeout" 2>&1 &
    tpid=$!

    i=0
    while [ "$i" -lt 50 ]; do
        if curl -sS --max-time 1 -o /dev/null "$TBASE/" 2>/dev/null; then
            break
        fi
        i=$((i + 1))
        sleep 0.1
    done

    check_status_slow "CGI timeout -> 504" "$TBASE/cgi-bin/sleep" "504"
    check_status_slow "CGI ignores SIGTERM -> 504" "$TBASE/cgi-bin/term" "504"
    check_status "server alive after CGI timeout" "$TBASE/" "200"

    kill -INT "$tpid" 2>/dev/null || true
    wait "$tpid" 2>/dev/null || true
    tpid=""
fi

long_out=$({ printf 'GET / HTTP/1.1\r\nHost: x\r\nX: '; \
    head -c 17000 /dev/zero | tr '\0' 'a'; \
    printf '\r\n\r\n'; } | nc -w 3 127.0.0.1 "$PORT" 2>/dev/null || true)
case "$long_out" in
    *"431"*)
        echo "PASS: oversized header -> 431"
        ;;
    *)
        echo "FAIL: oversized header (expected 431)"
        fail=1
        ;;
esac

uri_out=$({ printf 'GET /'; head -c 20000 /dev/zero | tr '\0' 'a'; \
    printf ' HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; } \
    | nc -w 3 127.0.0.1 "$PORT" 2>/dev/null || true)
case "$uri_out" in
    *"414"*)
        echo "PASS: 20KB URI -> 414"
        ;;
    *)
        echo "FAIL: 20KB URI (expected 414)"
        fail=1
        ;;
esac

# Graceful shutdown: SIGINT must stop the loop even with no traffic.
kill -INT "$pid" 2>/dev/null || true
i=0
while [ "$i" -lt 50 ] && process_alive "$pid"; do
    sleep 0.1
    i=$((i + 1))
done
if process_alive "$pid"; then
    echo "FAIL: SIGINT shutdown (still running after 5s)"
    fail=1
    kill -9 "$pid" 2>/dev/null || true
else
    echo "PASS: SIGINT shutdown"
fi
wait "$pid" 2>/dev/null || true
pid=""

if [ "$fail" -ne 0 ]; then
    echo "--- server_ev log ---"
    cat "$LOG"
    if [ -f "$LOG.timeout" ]; then
        echo "--- server_ev timeout log ---"
        cat "$LOG.timeout"
    fi
fi

exit "$fail"
