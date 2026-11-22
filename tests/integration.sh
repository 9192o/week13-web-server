#!/bin/sh
set -eu

PORT="${PORT:-18080}"
LOG="${TMPDIR:-/tmp}/web_server_integration.$$.log"
TESTROOT="${TMPDIR:-/tmp}/web_server_docroot.$$"

mkdir -p "$TESTROOT/dir" "$TESTROOT/empty"
cp www/index.html "$TESTROOT/index.html"
printf '<html><body>dir index</body></html>\n' > "$TESTROOT/dir/index.html"
printf 'SECRET=1\n' > "$TESTROOT/.env"
printf 'top secret\n' > "$TESTROOT/secret.txt"
chmod 000 "$TESTROOT/secret.txt"
ln -s index.html "$TESTROOT/link-inside"
ln -s /etc/passwd "$TESTROOT/link-outside"
dd if=/dev/zero of="$TESTROOT/big.bin" bs=1024 count=2048 >/dev/null 2>&1

./server "$PORT" "$TESTROOT" ./cgi-bin \
    --keepalive-idle 1 --keepalive-max 3 >"$LOG" 2>&1 &
pid=$!
timeout_pid=""

cleanup() {
    if [ -n "$timeout_pid" ]; then
        kill -INT "$timeout_pid" 2>/dev/null || true
        wait "$timeout_pid" 2>/dev/null || true
    fi
    kill -INT "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -rf "$TESTROOT"
}
trap cleanup EXIT INT TERM

# 서버가 뜰 때까지 짧게 기다린다.
i=0
while [ "$i" -lt 50 ]; do
    if curl -sS --max-time 1 -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done

fail=0

check_status() {
    desc="$1"
    url="$2"
    expected="$3"
    code=$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' "$url" 2>/dev/null || echo "000")
    if [ "$code" != "$expected" ]; then
        echo "FAIL: $desc (expected $expected, got $code)"
        fail=1
    else
        echo "PASS: $desc ($code)"
    fi
}

check_header() {
    desc="$1"
    url="$2"
    expected="$3"
    headers=$(curl -sS --max-time 2 -D - -o /dev/null "$url" 2>/dev/null || true)
    case "$headers" in
        *"$expected"*)
            echo "PASS: $desc"
            ;;
        *)
            echo "FAIL: $desc (missing '$expected')"
            fail=1
            ;;
    esac
}

check_content_type() {
    desc="$1"
    url="$2"
    expected="$3"
    ctype=$(curl -sS --max-time 2 -o /dev/null -w '%{content_type}' "$url" 2>/dev/null || echo "")
    case "$ctype" in
        "$expected"*)
            echo "PASS: $desc ($ctype)"
            ;;
        *)
            echo "FAIL: $desc (expected $expected*, got '$ctype')"
            fail=1
            ;;
    esac
}

check_body_contains() {
    desc="$1"
    url="$2"
    needle="$3"
    body=$(curl -sS --max-time 2 "$url" 2>/dev/null || echo "")
    case "$body" in
        *"$needle"*)
            echo "PASS: $desc"
            ;;
        *)
            echo "FAIL: $desc (body does not contain '$needle')"
            fail=1
            ;;
    esac
}

check_status_path_as_is() {
    desc="$1"
    path="$2"
    expected="$3"
    code=$(curl -sS --path-as-is --max-time 2 -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:$PORT$path" 2>/dev/null || echo "000")
    if [ "$code" != "$expected" ]; then
        echo "FAIL: $desc (expected $expected, got $code)"
        fail=1
    else
        echo "PASS: $desc ($code)"
    fi
}

check_head() {
    desc="$1"
    url="$2"
    headers=$(curl -sS -I --max-time 2 "$url" 2>/dev/null || true)
    case "$headers" in
        *"HTTP/1.1 200 OK"*"Content-Length:"*)
            echo "PASS: $desc"
            ;;
        *)
            echo "FAIL: $desc (HEAD 응답 헤더 이상)"
            fail=1
            ;;
    esac
}

check_status "GET /" "http://127.0.0.1:$PORT/" "200"
check_status "GET /index.html" "http://127.0.0.1:$PORT/index.html" "200"
check_status "GET /missing" "http://127.0.0.1:$PORT/missing" "404"
check_content_type "content-type /index.html" "http://127.0.0.1:$PORT/index.html" "text/html"
check_body_contains "body /index.html" "http://127.0.0.1:$PORT/index.html" "It works!"

# D12: 디렉토리/301/index/dotfile/symlink
check_status "GET /dir" "http://127.0.0.1:$PORT/dir" "301"
check_header "Location /dir/" "http://127.0.0.1:$PORT/dir" "Location: /dir/"
check_status "GET /dir/" "http://127.0.0.1:$PORT/dir/" "200"
check_body_contains "body /dir/" "http://127.0.0.1:$PORT/dir/" "dir index"
check_status "GET /empty/" "http://127.0.0.1:$PORT/empty/" "404"
check_status "GET /secret.txt" "http://127.0.0.1:$PORT/secret.txt" "403"
check_status "GET /.env" "http://127.0.0.1:$PORT/.env" "404"
check_status "GET /link-inside" "http://127.0.0.1:$PORT/link-inside" "200"
check_body_contains "body /link-inside" "http://127.0.0.1:$PORT/link-inside" "It works!"
check_status "GET /link-outside" "http://127.0.0.1:$PORT/link-outside" "403"

# 경로 탈출 / HEAD / Connection
check_status_path_as_is "GET /../etc/passwd" "/../etc/passwd" "403"
check_status_path_as_is "GET /%2e%2e/etc/passwd" "/%2e%2e/etc/passwd" "403"
check_head "HEAD /index.html" "http://127.0.0.1:$PORT/index.html"
check_header "Connection: keep-alive" "http://127.0.0.1:$PORT/index.html" "Connection: keep-alive"

# 큰 파일 스트리밍
big_code=$(curl -sS --max-time 5 -o "$TESTROOT/big.out" -w '%{http_code}' \
    "http://127.0.0.1:$PORT/big.bin" 2>/dev/null || echo "000")
big_len=$(wc -c < "$TESTROOT/big.out" 2>/dev/null | tr -d ' ' || echo "0")
if [ "$big_code" = "200" ] && [ "$big_len" = "2097152" ]; then
    echo "PASS: large file streaming (2MB)"
else
    echo "FAIL: large file streaming (code=$big_code, len=$big_len)"
    fail=1
fi

# 클라이언트가 응답 도중 끊겨도 서버가 살아있는지
curl -sS --max-time 1 --limit-rate 1k -o /dev/null \
    "http://127.0.0.1:$PORT/big.bin" >/dev/null 2>&1 &
dc_pid=$!
sleep 0.2
kill -9 "$dc_pid" 2>/dev/null || true
wait "$dc_pid" 2>/dev/null || true
sleep 0.2
check_status "server alive after client disconnect" \
    "http://127.0.0.1:$PORT/index.html" "200"

# D10: Last-Modified / If-Modified-Since
last_modified=$(curl -sS --max-time 2 -D - -o /dev/null \
    "http://127.0.0.1:$PORT/index.html" 2>/dev/null \
    | tr -d '\r' \
    | awk -F': ' 'tolower($1)=="last-modified"{print $2; exit}')

if [ -n "$last_modified" ]; then
    echo "PASS: Last-Modified header ($last_modified)"
else
    echo "FAIL: Last-Modified header missing"
    fail=1
fi

if [ -n "$last_modified" ]; then
    code=$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' \
        -H "If-Modified-Since: $last_modified" \
        "http://127.0.0.1:$PORT/index.html" 2>/dev/null || echo "000")
    if [ "$code" = "304" ]; then
        echo "PASS: If-Modified-Since -> 304"
    else
        echo "FAIL: If-Modified-Since (expected 304, got $code)"
        fail=1
    fi

    body_len=$(curl -sS --max-time 2 \
        -H "If-Modified-Since: $last_modified" \
        "http://127.0.0.1:$PORT/index.html" 2>/dev/null | wc -c | tr -d ' ')
    if [ "$body_len" = "0" ]; then
        echo "PASS: 304 body empty"
    else
        echo "FAIL: 304 body not empty (got $body_len bytes)"
        fail=1
    fi
fi

code=$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' \
    -H "If-Modified-Since: Wed, 21 Oct 2015 07:28:00 GMT" \
    "http://127.0.0.1:$PORT/index.html" 2>/dev/null || echo "000")
if [ "$code" = "200" ]; then
    echo "PASS: older If-Modified-Since -> 200"
else
    echo "FAIL: older If-Modified-Since (expected 200, got $code)"
    fail=1
fi

# D10: OPTIONS
headers=$(curl -sS -X OPTIONS --max-time 2 -D - -o /dev/null \
    "http://127.0.0.1:$PORT/index.html" 2>/dev/null || true)
case "$headers" in
    *"HTTP/1.1 200 OK"*"Allow: GET, POST, HEAD, OPTIONS"*)
        echo "PASS: OPTIONS /index.html"
        ;;
    *)
        echo "FAIL: OPTIONS /index.html (missing 200 + Allow)"
        fail=1
        ;;
esac

# D10: Range / 206
size=$(wc -c < "$TESTROOT/index.html" | tr -d ' ')
range_body="$TESTROOT/range_body"

headers=$(curl -sS -r 0-4 -D - -o "$range_body" \
    "http://127.0.0.1:$PORT/index.html" 2>/dev/null || true)
case "$headers" in
    *"HTTP/1.1 206 Partial Content"*)
        echo "PASS: Range 0-4 -> 206"
        ;;
    *)
        echo "FAIL: Range 0-4 (expected 206)"
        fail=1
        ;;
esac
case "$headers" in
    *"Content-Range: bytes 0-4/$size"*)
        echo "PASS: Content-Range 0-4/$size"
        ;;
    *)
        echo "FAIL: Content-Range 0-4/$size"
        fail=1
        ;;
esac
case "$headers" in
    *"Accept-Ranges: bytes"*)
        echo "PASS: Accept-Ranges: bytes"
        ;;
    *)
        echo "FAIL: Accept-Ranges: bytes"
        fail=1
        ;;
esac
range_len=$(wc -c < "$range_body" | tr -d ' ')
if [ "$range_len" = "5" ]; then
    echo "PASS: Range body length 5"
else
    echo "FAIL: Range body length (expected 5, got $range_len)"
    fail=1
fi

headers=$(curl -sS -r -5 -D - -o "$range_body" \
    "http://127.0.0.1:$PORT/index.html" 2>/dev/null || true)
case "$headers" in
    *"HTTP/1.1 206 Partial Content"*"Content-Range: bytes $((size - 5))-$((size - 1))/$size"*)
        echo "PASS: suffix Range -5"
        ;;
    *)
        echo "FAIL: suffix Range -5"
        fail=1
        ;;
esac

headers=$(curl -sS -r 999999- -D - -o /dev/null \
    "http://127.0.0.1:$PORT/index.html" 2>/dev/null || true)
case "$headers" in
    *"HTTP/1.1 416"*"Content-Range: bytes */$size"*)
        echo "PASS: invalid Range -> 416"
        ;;
    *)
        echo "FAIL: invalid Range (expected 416 + Content-Range)"
        fail=1
        ;;
esac

# D11: URI / 헤더 / 본문 한계
long_path=$(awk 'BEGIN{for(i=0;i<5000;i++)printf "a"}')
code=$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$PORT/$long_path" 2>/dev/null || echo "000")
if [ "$code" = "414" ]; then
    echo "PASS: long URI -> 414"
else
    echo "FAIL: long URI (expected 414, got $code)"
    fail=1
fi

header_out=$({ printf 'GET / HTTP/1.1\r\nHost: x\r\nX-Long: '; \
    awk 'BEGIN{for(i=0;i<17000;i++)printf "a"}'; \
    printf '\r\n\r\n'; } | nc -w 2 127.0.0.1 "$PORT" 2>/dev/null || true)
case "$header_out" in
    *"431"*)
        echo "PASS: oversized header block -> 431"
        ;;
    *)
        echo "FAIL: oversized header block (expected 431)"
        fail=1
        ;;
esac

many_out=$({ printf 'GET / HTTP/1.1\r\nHost: x\r\n'; \
    i=0; while [ "$i" -lt 101 ]; do \
        printf 'X-%d: a\r\n' "$i"; i=$((i + 1)); \
    done; printf '\r\n'; } | nc -w 2 127.0.0.1 "$PORT" 2>/dev/null || true)
case "$many_out" in
    *"431"*)
        echo "PASS: too many headers -> 431"
        ;;
    *)
        echo "FAIL: too many headers (expected 431)"
        fail=1
        ;;
esac

body_out=$(printf 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 8388609\r\n\r\n' \
    | nc -w 2 127.0.0.1 "$PORT" 2>/dev/null || true)
case "$body_out" in
    *"413"*)
        echo "PASS: body too large -> 413"
        ;;
    *)
        echo "FAIL: body too large (expected 413)"
        fail=1
        ;;
esac

post_no_cl_out=$(printf 'POST / HTTP/1.1\r\nHost: x\r\n\r\n' \
    | nc -w 2 127.0.0.1 "$PORT" 2>/dev/null || true)
case "$post_no_cl_out" in
    *"411"*)
        echo "PASS: POST without Content-Length -> 411"
        ;;
    *)
        echo "FAIL: POST without Content-Length (expected 411)"
        fail=1
        ;;
esac

# D11: 느린 요청 타임아웃 (별도 서버, --timeout 1)
TIMEOUT_PORT=$((PORT + 1))
TIMEOUT_LOG="${TMPDIR:-/tmp}/web_server_timeout.$$.log"
./server "$TIMEOUT_PORT" "$TESTROOT" ./cgi-bin --timeout 1 >"$TIMEOUT_LOG" 2>&1 &
timeout_pid=$!

i=0
while [ "$i" -lt 50 ]; do
    if curl -sS --max-time 1 -o /dev/null \
        "http://127.0.0.1:$TIMEOUT_PORT/" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done

slow_out=$({ printf 'GET / HTTP/1.1\r\nHost: x\r\n'; sleep 2; printf '\r\n'; } \
    | nc -w 3 127.0.0.1 "$TIMEOUT_PORT" 2>/dev/null || true)
case "$slow_out" in
    *"408 Request Timeout"*)
        echo "PASS: slow request -> 408"
        ;;
    *)
        echo "FAIL: slow request (expected 408)"
        fail=1
        ;;
esac

# CGI timeout / crash (timeout 서버: --timeout 1)
code=$(curl -sS --max-time 3 -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$TIMEOUT_PORT/cgi-bin/sleep" 2>/dev/null || echo "000")
if [ "$code" = "504" ]; then
    echo "PASS: CGI timeout -> 504"
else
    echo "FAIL: CGI timeout (expected 504, got $code)"
    fail=1
fi

code=$(curl -sS --max-time 3 -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$TIMEOUT_PORT/cgi-bin/crash" 2>/dev/null || echo "000")
if [ "$code" = "500" ]; then
    echo "PASS: CGI crash -> 500"
else
    echo "FAIL: CGI crash (expected 500, got $code)"
    fail=1
fi

kill -INT "$timeout_pid" 2>/dev/null || true
wait "$timeout_pid" 2>/dev/null || true
timeout_pid=""

# CGI: GET / POST / env / 404
check_status "GET /cgi-bin/echo?x=1" \
    "http://127.0.0.1:$PORT/cgi-bin/echo?x=1" "200"
check_body_contains "CGI GET method" \
    "http://127.0.0.1:$PORT/cgi-bin/echo?x=1" "METHOD=GET"
check_body_contains "CGI GET query" \
    "http://127.0.0.1:$PORT/cgi-bin/echo?x=1" "QUERY=x=1"
check_body_contains "CGI SCRIPT_NAME" \
    "http://127.0.0.1:$PORT/cgi-bin/echo?x=1" "SCRIPT_NAME=/cgi-bin/echo"
check_body_contains "CGI SERVER_PROTOCOL" \
    "http://127.0.0.1:$PORT/cgi-bin/echo?x=1" "SERVER_PROTOCOL=HTTP/1.1"
check_body_contains "CGI REMOTE_ADDR" \
    "http://127.0.0.1:$PORT/cgi-bin/echo?x=1" "REMOTE_ADDR=127.0.0.1"

code=$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' \
    -d 'hello=world' "http://127.0.0.1:$PORT/cgi-bin/echo" 2>/dev/null || echo "000")
if [ "$code" = "200" ]; then
    echo "PASS: POST /cgi-bin/echo (200)"
else
    echo "FAIL: POST /cgi-bin/echo (expected 200, got $code)"
    fail=1
fi

cgi_body=$(curl -sS --max-time 2 -d 'hello=world' \
    "http://127.0.0.1:$PORT/cgi-bin/echo" 2>/dev/null || echo "")
case "$cgi_body" in
    *"METHOD=POST"*"CONTENT_LENGTH=11"*"BODY=hello=world"*)
        echo "PASS: CGI POST body"
        ;;
    *)
        echo "FAIL: CGI POST body"
        fail=1
        ;;
esac

check_status "GET /cgi-bin/nope" \
    "http://127.0.0.1:$PORT/cgi-bin/nope" "404"

# D9: keep-alive
ka_out=$({ printf 'GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n'; \
    printf 'GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; } \
    | nc -w 3 127.0.0.1 "$PORT" 2>/dev/null || true)
ka_count=$(printf '%s' "$ka_out" | grep -c 'HTTP/1.1 200 OK' || true)
if [ "$ka_count" = "2" ]; then
    echo "PASS: keep-alive 2 requests on one connection"
else
    echo "FAIL: keep-alive (expected 2 responses, got $ka_count)"
    fail=1
fi
case "$ka_out" in
    *"Connection: keep-alive"*)
        echo "PASS: keep-alive response header"
        ;;
    *)
        echo "FAIL: keep-alive response header missing"
        fail=1
        ;;
esac

h10_out=$(printf 'GET / HTTP/1.0\r\n\r\n' \
    | nc -w 2 127.0.0.1 "$PORT" 2>/dev/null || true)
case "$h10_out" in
    *"Connection: close"*)
        echo "PASS: HTTP/1.0 default close"
        ;;
    *)
        echo "FAIL: HTTP/1.0 default close"
        fail=1
        ;;
esac

h10k_out=$({ printf 'GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n'; \
    printf 'GET / HTTP/1.0\r\nConnection: close\r\n\r\n'; } \
    | nc -w 3 127.0.0.1 "$PORT" 2>/dev/null || true)
h10k_count=$(printf '%s' "$h10k_out" | grep -c '200 OK' || true)
if [ "$h10k_count" = "2" ]; then
    echo "PASS: HTTP/1.0 keep-alive"
else
    echo "FAIL: HTTP/1.0 keep-alive (expected 2 responses, got $h10k_count)"
    fail=1
fi
case "$h10k_out" in
    *"Connection: keep-alive"*)
        echo "PASS: HTTP/1.0 keep-alive header"
        ;;
    *)
        echo "FAIL: HTTP/1.0 keep-alive header"
        fail=1
        ;;
esac

h11_close_out=$(printf 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' \
    | nc -w 2 127.0.0.1 "$PORT" 2>/dev/null || true)
case "$h11_close_out" in
    *"Connection: close"*)
        echo "PASS: HTTP/1.1 Connection: close"
        ;;
    *)
        echo "FAIL: HTTP/1.1 Connection: close"
        fail=1
        ;;
esac

max_out=$({ for _ in 1 2 3 4; do \
    printf 'GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n'; \
done; } | nc -w 3 127.0.0.1 "$PORT" 2>/dev/null || true)
max_count=$(printf '%s' "$max_out" | grep -c 'HTTP/1.1 200 OK' || true)
if [ "$max_count" = "3" ]; then
    echo "PASS: keep-alive max requests = 3"
else
    echo "FAIL: keep-alive max requests (expected 3, got $max_count)"
    fail=1
fi

idle_out=$({ printf 'GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n'; \
    sleep 2; \
    printf 'GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; } \
    | nc -w 3 127.0.0.1 "$PORT" 2>/dev/null || true)
idle_count=$(printf '%s' "$idle_out" | grep -c 'HTTP/1.1 200 OK' || true)
if [ "$idle_count" = "1" ]; then
    echo "PASS: keep-alive idle timeout closes connection"
else
    echo "FAIL: keep-alive idle timeout (expected 1 response, got $idle_count)"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "--- server log ---"
    cat "$LOG"
fi

exit "$fail"
