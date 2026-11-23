#!/bin/bash
#
# 기능 테스트: 서버를 임시 문서 루트로 띄우고 curl / raw 요청으로 시나리오를 확인한다.
#
#   사용법: tests/functional.sh <서버 바이너리> [--slow]
#     --slow  시간 제한 테스트(408, 무응답 close, CGI 시간 초과, 처리 중 SIGINT)까지 실행
#     PORT    환경 변수로 포트 변경 (기본 18099)
#
# 실패가 있거나, SIGTERM 종료 코드가 0 이 아니거나, 서버 로그에 sanitizer 보고가
# 있으면 exit 1. 필요 도구: bash(/dev/tcp), curl, coreutils(head, cmp, timeout).

set -u

BIN=${1:?usage: tests/functional.sh <server binary> [--slow]}
SLOW=0
[ "${2:-}" = "--slow" ] && SLOW=1
PORT=${PORT:-18099}
URL=http://127.0.0.1:$PORT
REPO=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
ADDER=$REPO/www/cgi-bin/adder

checks=0
failures=0
SRV=

# ---------------------------------------------------------------- 판정 도우미

ok()   { checks=$((checks + 1)); echo "ok   $1"; }
fail() { checks=$((checks + 1)); failures=$((failures + 1)); echo "FAIL $1 ($2)"; }

# expect 이름 기대값 실제값
expect() {
    if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "기대 [$2], 실제 [$3]"; fi
}

# expect_match 이름 정규식(grep -E, 줄 단위) 실제값
expect_match() {
    if printf '%s\n' "$3" | grep -qE -- "$2"; then
        ok "$1"
    else
        fail "$1" "기대 /$2/, 실제 [$(printf '%s' "$3" | head -c 200)]"
    fi
}

# expect_between 이름 최소 최대 실제값 (정수)
expect_between() {
    if [ "$4" -ge "$2" ] && [ "$4" -le "$3" ]; then
        ok "$1"
    else
        fail "$1" "기대 $2..$3, 실제 $4"
    fi
}

log_has() { grep -qF -- "$1" "$LOG"; }

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

# raw 요청(\r\n 은 이스케이프로)을 보내고 응답의 첫 줄만 출력
raw_status() {
    timeout 20 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$PORT"'
printf "%b" "$1" >&3
head -1 <&3' _ "$1" | tr -d '\r'
}

repeat() { head -c "$2" /dev/zero | tr '\0' "$1"; }

# ---------------------------------------------------------------- 임시 문서 루트

WORK=$(mktemp -d /tmp/ws-ftest-XXXXXX)
R=$WORK/www
LOG=$WORK/server.log

cleanup() {
    if [ -n "$SRV" ] && kill -0 "$SRV" 2>/dev/null; then
        kill -KILL "$SRV" 2>/dev/null
        wait "$SRV" 2>/dev/null
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ ! -x "$ADDER" ]; then
    echo "adder 가 없음: $ADDER (make 먼저)" >&2
    exit 1
fi

mkdir -p "$R/empty" "$R/docs" "$R/cgi-bin"
echo '<h1>hi</h1>' > "$R/index.html"
echo docs > "$R/docs/index.html"
head -c 4096 /dev/urandom > "$R/cat.png"
head -c 104857600 /dev/zero > "$R/big.bin"
echo KEY=1 > "$R/.env"
echo secret > "$WORK/secret.txt"
ln -s ../secret.txt "$R/escape"
head -c 800000 /dev/zero | tr '\0' q > "$WORK/800k"
cp "$ADDER" "$R/cgi-bin/adder"

# cgi 이름 셸본문 : #!/bin/sh 테스트 CGI
cgi() {
    printf '#!/bin/sh\n%s\n' "$2" > "$R/cgi-bin/$1"
    chmod 755 "$R/cgi-bin/$1"
}
cgi crash     'printf "Content-Type: text/plain\n\npartial"; kill -SEGV $$'
cgi made      'printf "Status: 201 Whatever\nContent-Type: text/plain\n\nmade"'
cgi nocontent 'printf "Status: 204\nContent-Type: text/plain\n\n"'
cgi slow      'sleep 10'
cgi len       'printf "Content-Type: text/plain\n\n"; wc -c'
cgi noexec    'printf "Content-Type: text/plain\n\nx"'
chmod 644 "$R/cgi-bin/noexec"

# ---------------------------------------------------------------- 서버 시작

"$BIN" -p "$PORT" -r "$R" 2>"$LOG" &
SRV=$!
for _ in $(seq 50); do
    [ "$(code "$URL/index.html")" = 200 ] && break
    sleep 0.1
done
if [ "$(code "$URL/index.html")" != 200 ]; then
    echo "서버가 시작되지 않음 (포트 $PORT 사용 중?)" >&2
    cat "$LOG" >&2
    exit 1
fi

# ---------------------------------------------------------------- 정적 파일

echo "== 정적 파일"
H=$(curl -si "$URL/index.html" | tr -d '\r')
expect_match "S1 GET /index.html → 200"        '^HTTP/1.1 200 OK$' "$H"
expect_match "S1 Content-Type text/html"        '^Content-Type: text/html; charset=utf-8$' "$H"
expect_match "S1 Content-Length 12"             '^Content-Length: 12$' "$H"
expect_match "S1 Connection: close"             '^Connection: close$' "$H"
expect_match "S1 Date (IMF-fixdate)"            '^Date: [A-Z][a-z]{2}, [0-9]{2} [A-Z][a-z]{2} [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT$' "$H"
expect "S2 /nope.html → 404"                    404 "$(code "$URL/nope.html")"
expect "S3 /cat.png → 200"                      200 "$(code "$URL/cat.png")"
expect "S3 Content-Type image/png"              image/png "$(curl -s -o /dev/null -w '%{content_type}' "$URL/cat.png")"
if curl -s "$URL/cat.png" | cmp -s - "$R/cat.png"; then
    ok "S3 바이너리 본문 동일"
else
    fail "S3 바이너리 본문 동일" "내용이 다름"
fi
expect "S4-1 index.html 없는 디렉터리 → 403"    403 "$(code "$URL/empty/")"
expect "S4-2 .env → 404"                        404 "$(code "$URL/.env")"
H=$(curl -si "$URL/docs?x=1" | tr -d '\r')
expect_match "끝 / 없는 디렉터리 → 301"         '^HTTP/1.1 301 Moved Permanently$' "$H"
expect_match "301 Location 에 / 와 쿼리"        '^Location: /docs/\?x=1$' "$H"
expect "/docs/ → 200"                           200 "$(code "$URL/docs/")"
expect "/../secret.txt → 400"                   400 "$(code --path-as-is "$URL/../secret.txt")"
expect "/%2e%2e/secret.txt → 400"               400 "$(code --path-as-is "$URL/%2e%2e/secret.txt")"
expect "root 밖 심볼릭 링크 → 403"              403 "$(code "$URL/escape")"
expect "PATH_MAX 넘는 경로 → 500"               500 "$(code "$URL/$(repeat a 5000)")"
H=$(curl -si -d a=1 "$URL/index.html" | tr -d '\r')
expect_match "정적 경로 POST → 405"             '^HTTP/1.1 405 Method Not Allowed$' "$H"
expect_match "405 Allow: GET"                   '^Allow: GET$' "$H"

# ---------------------------------------------------------------- 요청 파싱

echo "== 요청 파싱"
expect "S7 DELETE → 501"                        501 "$(code -X DELETE "$URL/index.html")"
expect "S8 9000자 URI → 414"                    414 "$(code "$URL/$(repeat a 9000)")"
expect "20000자 URI (버퍼 초과) → 414"          414 "$(code "$URL/$(repeat a 20000)")"
expect "20KB Cookie → 431"                      431 "$(code -H "Cookie: $(repeat c 20000)" "$URL/")"
expect "HTTP/2.0 → 505"                         "HTTP/1.1 505 HTTP Version Not Supported" \
       "$(raw_status 'GET / HTTP/2.0\r\nHost: x\r\n\r\n')"
expect "HTTP/1.1 Host 없음 → 400"               "HTTP/1.1 400 Bad Request" \
       "$(raw_status 'GET / HTTP/1.1\r\n\r\n')"
expect "Transfer-Encoding: chunked → 501"       "HTTP/1.1 501 Not Implemented" \
       "$(raw_status 'POST /cgi-bin/len HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n')"

# ---------------------------------------------------------------- POST 본문

echo "== POST 본문"
expect "Content-Length 없음 → 411"              "HTTP/1.1 411 Length Required" \
       "$(raw_status 'POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\n\r\n')"
expect "Content-Length 2000000 → 413"           "HTTP/1.1 413 Content Too Large" \
       "$(raw_status 'POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n')"
OUT=$(curl -sv -H 'Expect: 100-continue' --data-binary @"$WORK/800k" "$URL/cgi-bin/len" 2>&1 | tr -d '\r')
expect_match "Expect: 100-continue → 100 Continue" '^< HTTP/1.1 100 Continue$' "$OUT"
expect_match "800KB 본문이 CGI stdin 으로 전달"    '^ *800000$' "$OUT"
OUT=$(timeout 10 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$PORT"'
printf "POST /cgi-bin/len HTTP/1.0\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n" >&3
early=$(timeout 1 head -c 30 <&3 | tr -d "\r\n")
printf hello >&3
echo "early=[$early] $(head -1 <&3 | tr -d "\r")"')
expect "HTTP/1.0 + Expect 에는 100 을 안 보냄"  "early=[] HTTP/1.1 200 OK" "$OUT"
timeout 5 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$PORT"'
printf "POST /cgi-bin/len HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nab" >&3
exec 3>&-'
sleep 0.3
expect "S9 본문 도중 끊김 뒤에도 서버 정상"      200 "$(code "$URL/index.html")"
if log_has "(client closed mid-body)"; then ok "S9 로그 (client closed mid-body)"; else fail "S9 로그 (client closed mid-body)" "로그에 없음"; fi

# ---------------------------------------------------------------- CGI

echo "== CGI"
H=$(curl -si "$URL/cgi-bin/adder?a=1&b=2" | tr -d '\r')
expect_match "S5 GET adder?a=1&b=2 → 200"       '^HTTP/1.1 200 OK$' "$H"
expect_match "S5 Content-Type text/plain"        '^Content-Type: text/plain; charset=utf-8$' "$H"
expect "S5 본문 3"                               3 "$(curl -s "$URL/cgi-bin/adder?a=1&b=2")"
expect "S6 POST a=1&b=2 → 3"                     3 "$(curl -s -d 'a=1&b=2' "$URL/cgi-bin/adder")"
expect "adder 음수 a=-5&b=2 → -3"                -3 "$(curl -s "$URL/cgi-bin/adder?a=-5&b=2")"
expect "adder 잘못된 입력 → 400"                 400 "$(code "$URL/cgi-bin/adder?a=x&b=2")"
H=$(curl -si "$URL/cgi-bin/crash" | tr -d '\r')
expect_match "S11 CGI 크래시 → 500"             '^HTTP/1.1 500 Internal Server Error$' "$H"
if printf '%s' "$H" | grep -q partial; then
    fail "S11 부분 출력은 버림" "partial 이 응답에 있음"
else
    ok "S11 부분 출력은 버림"
fi
if log_has "(cgi signal 11)"; then ok "S11 로그 (cgi signal 11)"; else fail "S11 로그 (cgi signal 11)" "로그에 없음"; fi
expect "Status: 201 → 빈 문구"                   "HTTP/1.1 201 " "$(curl -si "$URL/cgi-bin/made" | head -1 | tr -d '\r')"
expect "Status: 204 → 500"                       500 "$(code "$URL/cgi-bin/nocontent")"
expect "/cgi-bin/adder/extra → 404"              404 "$(code "$URL/cgi-bin/adder/extra")"
expect "실행 권한 없는 CGI → 403"                403 "$(code "$URL/cgi-bin/noexec")"
expect "없는 CGI 에 CL 없이 POST → 404"          "HTTP/1.1 404 Not Found" \
       "$(raw_status 'POST /cgi-bin/missing HTTP/1.1\r\nHost: x\r\n\r\n')"

# ---------------------------------------------------------------- 큰 파일

echo "== 큰 파일 (S10)"
expect "100MB 전체 수신"                         104857600 "$(curl -s "$URL/big.bin" | wc -c | tr -d ' ')"
GOT=$(curl -s "$URL/big.bin" | head -c 1000000 | wc -c | tr -d ' ')
expect "1MB 받고 끊음"                           1000000 "$GOT"
sleep 0.3
expect "S10 전송 중 끊김 뒤에도 서버 정상"       200 "$(code "$URL/index.html")"
if log_has "(aborted)"; then ok "S10 로그 (aborted)"; else fail "S10 로그 (aborted)" "로그에 없음"; fi

# ---------------------------------------------------------------- 시간 제한 (--slow)

if [ "$SLOW" = 1 ]; then
    echo "== 시간 제한 (--slow)"
    T0=$(now_ms)
    expect "헤더 일부만 보내고 대기 → 408"       "HTTP/1.1 408 Request Timeout" \
           "$(raw_status 'GET / HTTP/1.1\r\nHo')"
    expect_between "헤더 408 까지 약 5초 (ms)"   4500 7000 $(( $(now_ms) - T0 ))

    T0=$(now_ms)
    N=$(timeout 20 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$PORT"'; cat <&3 | wc -c' | tr -d ' ')
    expect "아무것도 안 보냄 → 응답 없이 close" 0 "$N"
    expect_between "무응답 close 까지 약 5초 (ms)" 4500 7000 $(( $(now_ms) - T0 ))

    T0=$(now_ms)
    expect "본문 일부만 보내고 대기 → 408"       "HTTP/1.1 408 Request Timeout" \
           "$(raw_status 'POST /cgi-bin/len HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nab')"
    expect_between "본문 408 까지 약 10초 (ms)"  9500 12000 $(( $(now_ms) - T0 ))

    T0=$(now_ms)
    expect "CGI 시간 초과 → 500"                 500 "$(code "$URL/cgi-bin/slow")"
    expect_between "CGI kill 까지 약 5초 (ms)"   4500 7000 $(( $(now_ms) - T0 ))
    if log_has "(cgi timeout)"; then ok "로그 (cgi timeout)"; else fail "로그 (cgi timeout)" "로그에 없음"; fi
fi

# ---------------------------------------------------------------- 종료

echo "== 종료"
if [ "$SLOW" = 1 ]; then
    # 처리 중인 요청이 있을 때 SIGINT: 그 요청을 끝까지 처리한 뒤 exit 0
    raw_status 'GET / HTTP/1.1\r\nHo' > "$WORK/inflight" &
    CLI=$!
    sleep 0.5
    kill -INT "$SRV"
    wait "$SRV"
    RC=$?
    wait "$CLI"
    SRV=
    expect "처리 중 SIGINT → 요청은 408 로 마무리" "HTTP/1.1 408 Request Timeout" "$(cat "$WORK/inflight")"
    expect "처리 중 SIGINT → exit 0"             0 "$RC"
else
    kill -TERM "$SRV"
    wait "$SRV"
    RC=$?
    SRV=
    expect "SIGTERM → exit 0"                    0 "$RC"
fi
if log_has "shutting down"; then ok "로그 shutting down"; else fail "로그 shutting down" "로그에 없음"; fi
expect "sanitizer 보고 없음"                     0 "$(grep -cE 'Sanitizer|runtime error' "$LOG")"

# ---------------------------------------------------------------- 요약

echo "$checks checks, $failures failures"
if [ "$failures" -ne 0 ]; then
    echo "---- server log (임시 디렉터리와 함께 삭제됨)"
    cat "$LOG"
    exit 1
fi
exit 0
