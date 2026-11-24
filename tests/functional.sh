#!/bin/bash
#
# 기능 테스트: 서버를 임시 문서 루트로 띄우고 curl / raw 요청으로 시나리오를 확인한다.
#
#   사용법: tests/functional.sh <서버 바이너리> [--timing]
#     --timing  시간 제한 시나리오(408, 무응답 close, CGI 시간 초과, 전송 시간 초과,
#               처리 중 SIGINT)까지 실행
#     PORT      환경 변수로 포트 변경 (기본 18099)
#     HEADER_MS BODY_MS CGI_MS SEND_MS KEEPALIVE_MS SHUTDOWN_MS
#               서버가 빌드된 시간 제한 값(ms). 기본은 실제 값 5000/10000/5000/30000/5000/10000.
#               make ftest 는 짧은 값으로 빌드한 서버와 함께 그 값을 넘긴다.
#
# 실패가 있거나, SIGTERM 종료 코드가 0 이 아니거나, 서버 로그에 sanitizer 보고가
# 있거나, 서버 fd 수가 시작 때와 다르거나, CGI 가 0/1/2 외의 fd 를 물려받았으면 exit 1.
# 필요 도구: bash(/dev/tcp), curl, coreutils(head, cmp, timeout, ls), /proc.

set -u

BIN=${1:?usage: tests/functional.sh <server binary> [--timing]}
TIMING=0
case "${2:-}" in
    "") ;;
    --timing) TIMING=1 ;;
    *) echo "알 수 없는 옵션: $2 (--timing 만 있음)" >&2; exit 2 ;;
esac
HEADER_MS=${HEADER_MS:-5000}
BODY_MS=${BODY_MS:-10000}
CGI_MS=${CGI_MS:-5000}
SEND_MS=${SEND_MS:-30000}
KEEPALIVE_MS=${KEEPALIVE_MS:-5000}
SHUTDOWN_MS=${SHUTDOWN_MS:-10000}
PORT=${PORT:-18099}
URL=http://127.0.0.1:$PORT
REPO=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
ADDER=$REPO/www/cgi-bin/adder

checks=0
failures=0
SRV=
EXTRA=()   # start_extra 로 더 띄운 서버들의 pid (cleanup 용)

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

# expect_ms 이름 시간제한값(ms) 실제경과(ms) : 값의 90% ~ 값+2초 사이면 통과
expect_ms() {
    expect_between "$1" $(( $2 * 9 / 10 )) $(( $2 + 2000 )) "$3"
}

ms_to_s()  { echo "$(( $1 / 1000 )).$(printf '%03d' $(( $1 % 1000 )))"; }
sleep_ms() { sleep "$(ms_to_s "$1")"; }

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

# raw 요청을 보내고 서버가 연결을 닫을 때까지 받은 전부를 출력 (\r 제거)
raw_all() {
    timeout 20 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$PORT"'
printf "%b" "$1" >&3
cat <&3' _ "$1" | tr -d '\r'
}

# ---------------------------------------------------------------- 임시 문서 루트

WORK=$(mktemp -d /tmp/ws-ftest-XXXXXX)
R=$WORK/www
LOG=$WORK/server.log

cleanup() {
    if [ -n "$SRV" ] && kill -0 "$SRV" 2>/dev/null; then
        kill -KILL "$SRV" 2>/dev/null
        wait "$SRV" 2>/dev/null
    fi
    for pid in "${EXTRA[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -KILL "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
        fi
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ ! -x "$ADDER" ]; then
    echo "adder 가 없음: $ADDER (make 먼저)" >&2
    exit 1
fi

mkdir -p "$R/empty" "$R/docs" "$R/cgi-bin"
echo '<h1>hi</h1>' > "$R/index.html"
echo 'h1{}' > "$R/style.css"
echo docs > "$R/docs/index.html"
head -c 4096 /dev/urandom > "$R/cat.png"
head -c 104857600 /dev/zero > "$R/big.bin"
head -c 10485760 /dev/urandom > "$R/mid.bin"   # P2-S5: 소켓 버퍼보다 크지만 금방 받는 크기

# start_extra 포트 로그 [서버 인자...] : 같은 문서 루트로 서버를 하나 더 띄운다 (pid 는 XPID).
# 메인 서버처럼 0/1/2 외의 fd 는 닫고 시작한다.
start_extra() {
    local port=$1 log=$2
    shift 2
    (
        for fd in $(ls "/proc/$BASHPID/fd"); do
            [ "$fd" -gt 2 ] && eval "exec $fd>&-"
        done
        exec "$BIN" -p "$port" -r "$R" "$@"
    ) 2>"$log" &
    XPID=$!
    EXTRA+=("$XPID")
    for _ in $(seq 50); do
        [ "$(code "http://127.0.0.1:$port/index.html")" = 200 ] && break
        sleep 0.1
    done
}
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
cgi fds       'printf "Content-Type: text/plain\n\n"; exec ls /proc/self/fd'
chmod 644 "$R/cgi-bin/noexec"

# ---------------------------------------------------------------- 서버 시작

# 서버는 0/1/2 만 가지고 시작한다. 셸이나 터미널이 물려준 fd(예: WSL 의 /dev/ptmx)가
# 있으면 닫는다 — 그래야 아래 fd 검사가 실행 환경 탓으로 흔들리지 않는다.
(
    for fd in $(ls "/proc/$BASHPID/fd"); do
        [ "$fd" -gt 2 ] && eval "exec $fd>&-"
    done
    exec "$BIN" -p "$PORT" -r "$R"
) 2>"$LOG" &
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

# 서버 프로세스의 열린 fd 수. 시작 직후 값을 기억해 두고 끝에서 비교한다.
fd_count() { ls "/proc/$SRV/fd" 2>/dev/null | wc -l | tr -d ' '; }

# 마지막 연결이 아직 닫히는 중일 수 있어, 0.1초 간격으로 두 번 같은 값이 나올 때까지
fd_settled() {
    local prev cur
    prev=$(fd_count)
    for _ in $(seq 20); do
        sleep 0.1
        cur=$(fd_count)
        [ "$cur" = "$prev" ] && break
        prev=$cur
    done
    echo "$cur"
}
FD0=$(fd_settled)

# ---------------------------------------------------------------- 동시 처리 (phase 2)

echo "== 동시 처리"
# P2-S1: A 가 연결만 열고 아무것도 안 보내는 동안 B 가 요청 → 기다리지 않고 200
exec 5<>/dev/tcp/127.0.0.1/"$PORT"
sleep 0.2                                   # A 가 워커에 잡힐 시간
T0=$(now_ms)
H=$(curl -s -D - -o "$WORK/p2s1.body" "$URL/index.html" | tr -d '\r')
EL=$(( $(now_ms) - T0 ))
exec 5>&-
expect_match "P2-S1 A 가 붙잡아도 B → 200"     '^HTTP/1.1 200 OK$' "$H"
expect_match "P2-S1 Content-Type text/html"     '^Content-Type: text/html; charset=utf-8$' "$H"
expect_match "P2-S1 Content-Length 12"          '^Content-Length: 12$' "$H"
if cmp -s "$WORK/p2s1.body" "$R/index.html"; then
    ok "P2-S1 본문 = index.html"
else
    fail "P2-S1 본문 = index.html" "내용이 다름"
fi
expect_between "P2-S1 B 응답 1초 안 (ms)"      0 1000 "$EL"

# ---------------------------------------------------------------- keep-alive (phase 2)

echo "== keep-alive"
# P2-S4: 같은 연결로 두 번 (curl 은 URL 여러 개를 한 연결로 보냄). num_connects 0 = 재사용
W='%{http_code}:%{num_connects}:%{content_type} '
K=$(curl -s -o /dev/null -o /dev/null -w "$W" "$URL/index.html" "$URL/style.css")
expect "P2-S4 index.html 다음 style.css 를 같은 연결로" \
       "200:1:text/html; charset=utf-8 200:0:text/css; charset=utf-8 " "$K"
expect "404 뒤에도 연결 유지" "404:1 200:0 " \
       "$(curl -s -o /dev/null -o /dev/null -w '%{http_code}:%{num_connects} ' \
              "$URL/nope.html" "$URL/index.html")"
for _ in $(seq 20); do log_has "(keep-alive closed: eof, 2 requests" && break; sleep 0.1; done
if log_has "(keep-alive closed: eof, 2 requests"; then
    ok "클라이언트가 닫으면 로그 (keep-alive closed: eof, 2 requests"
else
    fail "클라이언트가 닫으면 로그 (keep-alive closed: eof, 2 requests" "로그에 없음"
fi

# pipelining: 요청 두 개를 한 번에 → 응답 두 개가 순서대로 (두 번째는 Connection: close)
P=$(raw_all 'GET /index.html HTTP/1.1\r\nHost: x\r\n\r\nGET /style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
expect "pipelining GET+GET → 응답 2개"        2 "$(printf '%s\n' "$P" | grep -c '^HTTP/1.1 200 OK$')"
expect "pipelining 순서 (html → css)"          "text/html; charset=utf-8 text/css; charset=utf-8" \
       "$(printf '%s\n' "$P" | sed -n 's/^Content-Type: //p' | tr '\n' ' ' | sed 's/ $//')"
# POST 본문 뒤에 붙어 온 다음 요청도 처리
P=$(raw_all 'POST /cgi-bin/len HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhelloGET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
expect "pipelining POST+GET → 응답 2개"       2 "$(printf '%s\n' "$P" | grep -c '^HTTP/1.1 200 OK$')"
expect_match "pipelining POST 본문 5바이트"    '^5$' "$P"
expect_match "pipelining 뒤 GET 본문"          '^<h1>hi</h1>$' "$P"

# 연결을 닫는 경우: 응답에 Connection: close, idle 시간까지 기다리지 않고 바로 닫힘
T0=$(now_ms)
P=$(raw_all 'GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
EL=$(( $(now_ms) - T0 ))
expect_match "Connection: close 요청 → 응답 Connection: close" '^Connection: close$' "$P"
expect_between "Connection: close → 바로 닫힘 (ms)" 0 $(( KEEPALIVE_MS * 9 / 10 )) "$EL"
T0=$(now_ms)
P=$(raw_all 'GET /index.html HTTP/1.0\r\n\r\n')
EL=$(( $(now_ms) - T0 ))
expect_match "HTTP/1.0 → 응답 Connection: close" '^Connection: close$' "$P"
expect_between "HTTP/1.0 → 바로 닫힘 (ms)"     0 $(( KEEPALIVE_MS * 9 / 10 )) "$EL"
# 비CGI 요청에 본문이 있으면 (읽지 않으므로) 닫음
P=$(raw_all 'GET /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc')
expect_match "본문 있는 GET → Connection: close" '^Connection: close$' "$P"

# 최대 요청 수 100: 101개를 보내면 100번째 응답 뒤 닫히고 101번째는 새 연결
ARGS=()
for _ in $(seq 101); do ARGS+=(-o /dev/null "$URL/index.html"); done
M=$(curl -s -w '%{num_connects} ' "${ARGS[@]}")
expect "101개 요청 → 연결 2개"                 2 "$(echo $M | tr ' ' '\n' | grep -c '^1$')"
expect "101번째 요청이 새 연결"                1 "$(echo $M | awk '{print $101}')"
for _ in $(seq 20); do log_has "(keep-alive closed: max, 100 requests" && break; sleep 0.1; done
if log_has "(keep-alive closed: max, 100 requests"; then
    ok "로그 (keep-alive closed: max, 100 requests"
else
    fail "로그 (keep-alive closed: max, 100 requests" "로그에 없음"
fi

# ---------------------------------------------------------------- 정적 파일

echo "== 정적 파일"
H=$(curl -si "$URL/index.html" | tr -d '\r')
expect_match "S1 GET /index.html → 200"        '^HTTP/1.1 200 OK$' "$H"
expect_match "S1 Content-Type text/html"        '^Content-Type: text/html; charset=utf-8$' "$H"
expect_match "S1 Content-Length 12"             '^Content-Length: 12$' "$H"
if printf '%s\n' "$H" | grep -qi '^Connection:'; then   # HTTP/1.1 keep-alive 기본값
    fail "S1 Connection 헤더 없음 (keep-alive)" "$(printf '%s\n' "$H" | grep -i '^Connection:')"
else
    ok "S1 Connection 헤더 없음 (keep-alive)"
fi
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
# 100 이 오는지 지켜보는 시간: 본문 시간 제한보다 짧아야 그 사이 408 이 오지 않음
EARLY_MS=$(( BODY_MS / 2 < 1000 ? BODY_MS / 2 : 1000 ))
OUT=$(timeout 10 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$PORT"'
printf "POST /cgi-bin/len HTTP/1.0\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n" >&3
early=$(timeout '"$(ms_to_s "$EARLY_MS")"' head -c 30 <&3 | tr -d "\r\n")
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
# CGI 는 stdin/stdout/stderr 만 물려받아야 한다 (listen·연결 소켓, 파이프 반대편, 열어 둔 파일 X).
# 3 은 ls 가 /proc/self/fd 를 읽으려고 직접 연 디렉터리.
expect "CGI 가 물려받은 fd 는 0 1 2 뿐 (+ ls 자신의 3)" "0 1 2 3" \
       "$(curl -s "$URL/cgi-bin/fds" | tr '\n' ' ' | sed 's/ $//')"
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

# ---------------------------------------------------------------- 시간 제한 (--timing)

if [ "$TIMING" = 1 ]; then
    echo "== 시간 제한 (--timing: 헤더 ${HEADER_MS}ms, 본문 ${BODY_MS}ms, CGI ${CGI_MS}ms, 전송 ${SEND_MS}ms)"
    T0=$(now_ms)
    expect "헤더 일부만 보내고 대기 → 408"       "HTTP/1.1 408 Request Timeout" \
           "$(raw_status 'GET / HTTP/1.1\r\nHo')"
    expect_ms "헤더 408 까지 약 ${HEADER_MS}ms"  "$HEADER_MS" $(( $(now_ms) - T0 ))

    T0=$(now_ms)
    N=$(timeout 20 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$PORT"'; cat <&3 | wc -c' | tr -d ' ')
    expect "아무것도 안 보냄 → 응답 없이 close" 0 "$N"
    expect_ms "무응답 close 까지 약 ${HEADER_MS}ms" "$HEADER_MS" $(( $(now_ms) - T0 ))

    T0=$(now_ms)
    expect "본문 일부만 보내고 대기 → 408"       "HTTP/1.1 408 Request Timeout" \
           "$(raw_status 'POST /cgi-bin/len HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nab')"
    expect_ms "본문 408 까지 약 ${BODY_MS}ms"    "$BODY_MS" $(( $(now_ms) - T0 ))

    T0=$(now_ms)
    expect "CGI 시간 초과 → 500"                 500 "$(code "$URL/cgi-bin/slow")"
    expect_ms "CGI kill 까지 약 ${CGI_MS}ms"     "$CGI_MS" $(( $(now_ms) - T0 ))
    if log_has "(cgi timeout)"; then ok "로그 (cgi timeout)"; else fail "로그 (cgi timeout)" "로그에 없음"; fi

    # 응답을 읽지 않는 클라이언트: 소켓 버퍼(수 MB)가 찬 뒤 응답 전체 제한에서 끊김
    T0=$(now_ms)
    exec 4<>/dev/tcp/127.0.0.1/"$PORT"
    printf 'GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n' >&4
    for _ in $(seq $(( SEND_MS / 100 + 50 ))); do
        log_has "(send timeout)" && break
        sleep 0.1
    done
    EL=$(( $(now_ms) - T0 ))
    exec 4>&-
    if log_has "(send timeout)"; then
        ok "안 읽는 클라이언트 → 로그 (send timeout)"
    else
        fail "안 읽는 클라이언트 → 로그 (send timeout)" "로그에 없음"
    fi
    expect_ms "전송 끊김까지 약 ${SEND_MS}ms"     "$SEND_MS" "$EL"
    expect "전송 시간 초과 뒤에도 서버 정상"     200 "$(code "$URL/index.html")"

    # P2-S2: A 가 연결만 하고 아무것도 안 보냄 → A 는 약 HEADER_MS 뒤 응답 없이 끊기고,
    #        그동안 B 는 기다리지 않고 200
    T0=$(now_ms)
    timeout 20 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$PORT"'
cat <&3 2>/dev/null | wc -c' > "$WORK/p2s2.a" &
    A=$!
    sleep 0.2                               # A 가 워커에 잡힐 시간
    T1=$(now_ms)
    C=$(code "$URL/index.html")
    EB=$(( $(now_ms) - T1 ))
    wait "$A"
    EA=$(( $(now_ms) - T0 ))
    expect "P2-S2 B → 200"                       200 "$C"
    expect_between "P2-S2 B 응답 1초 안 (ms)"   0 1000 "$EB"
    expect "P2-S2 A 받은 바이트 0 (무응답 close)" 0 "$(tr -d ' ' < "$WORK/p2s2.a")"
    expect_ms "P2-S2 A 끊김까지 약 ${HEADER_MS}ms" "$HEADER_MS" "$EA"

    # slowloris: 헤더를 HEADER_MS/10 간격으로 1바이트씩 계속 보냄.
    # read 한 번 단위 제한(SO_RCVTIMEO)이면 영원히 안 끊기지만, 헤더 전체 데드라인이라 408.
    T0=$(now_ms)
    S=$(timeout 30 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$PORT"'
( printf "GET / HTTP/1.1\r\nHost: x\r\nX-Slow: " >&3
  for _ in $(seq 40); do printf a >&3 || exit 0; sleep '"$(ms_to_s $(( HEADER_MS / 10 )))"'; done ) 2>/dev/null &
head -1 <&3' | tr -d '\r')
    expect "slowloris (1바이트씩 계속) → 408"    "HTTP/1.1 408 Request Timeout" "$S"
    expect_ms "slowloris 408 까지 약 ${HEADER_MS}ms" "$HEADER_MS" $(( $(now_ms) - T0 ))

    # keep-alive 응답 속도: 한 연결로 20개. Nagle + 지연 ACK 가 걸리면 요청마다 ~40ms (800ms+)
    ARGS=()
    for _ in $(seq 20); do ARGS+=(-o /dev/null "$URL/index.html"); done
    T0=$(now_ms)
    K=$(curl -s -w '%{num_connects}' "${ARGS[@]}")
    EL=$(( $(now_ms) - T0 ))
    expect "keep-alive 20개를 한 연결로"          "10000000000000000000" "$K"
    expect_between "keep-alive 20개가 지연 ACK 없이 (ms, TCP_NODELAY)" 0 400 "$EL"

    # P2-S4: 응답 뒤 아무것도 안 보내면 약 KEEPALIVE_MS 뒤 응답 없이 닫힘 (408 없음)
    T0=$(now_ms)
    P=$(raw_all 'GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n')
    EL=$(( $(now_ms) - T0 ))
    expect "P2-S4 idle 닫힘: 응답은 200 하나뿐"   1 "$(printf '%s\n' "$P" | grep -c '^HTTP/1.1 ')"
    expect "P2-S4 idle 닫힘: 408 없음"             0 "$(printf '%s\n' "$P" | grep -c '408')"
    expect_ms "P2-S4 idle 닫힘까지 약 ${KEEPALIVE_MS}ms" "$KEEPALIVE_MS" "$EL"
    if log_has "(keep-alive closed: idle, 1 requests"; then
        ok "P2-S4 로그 (keep-alive closed: idle, 1 requests"
    else
        fail "P2-S4 로그 (keep-alive closed: idle, 1 requests" "로그에 없음"
    fi

    # P2-S3: 워커 2 / 큐 2 서버를 따로 띄워, 워커와 큐가 모두 찼을 때 503
    P3=$((PORT + 1))
    U3=http://127.0.0.1:$P3
    LOG3=$WORK/server3.log
    start_extra "$P3" "$LOG3" -w 2 -q 2
    SRV3=$XPID
    S3FD0=$(ls "/proc/$SRV3/fd" 2>/dev/null | wc -l | tr -d ' ')

    # hold 파일 : 연결만 하고 아무것도 안 보냄, 끊길 때까지 받은 바이트 수를 파일에
    hold() {
        timeout 20 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$P3"'
cat <&3 2>/dev/null | wc -c' > "$1"
    }
    T0=$(now_ms)
    hold "$WORK/a1" & A1=$!
    hold "$WORK/a2" & A2=$!
    sleep_ms $(( HEADER_MS / 5 ))          # 워커 2개가 A1, A2 를 붙잡을 시간 (A 가 끊기기 전에)
    curl -s -o "$WORK/c1.body" -w '%{http_code}' "$U3/index.html" > "$WORK/c1" & C1=$!
    curl -s -o "$WORK/c2.body" -w '%{http_code}' "$U3/index.html" > "$WORK/c2" & C2=$!
    sleep_ms $(( HEADER_MS / 5 ))          # C1, C2 가 큐 2칸을 채울 시간
    T1=$(now_ms)
    D=$(code "$U3/index.html")
    ED=$(( $(now_ms) - T1 ))
    wait "$C1" "$C2"
    EC=$(( $(now_ms) - T0 ))
    wait "$A1" "$A2"
    expect "P2-S3 D → 503"                        503 "$D"
    expect_between "P2-S3 D 응답 1초 안 (ms)"    0 1000 "$ED"
    if grep -qF "(queue full)" "$LOG3"; then
        ok "P2-S3 로그 (queue full)"
    else
        fail "P2-S3 로그 (queue full)" "로그에 없음"
    fi
    expect "P2-S3 C1 → 200"                       200 "$(cat "$WORK/c1")"
    expect "P2-S3 C2 → 200"                       200 "$(cat "$WORK/c2")"
    if cmp -s "$WORK/c1.body" "$R/index.html" && cmp -s "$WORK/c2.body" "$R/index.html"; then
        ok "P2-S3 C1, C2 본문 = index.html"
    else
        fail "P2-S3 C1, C2 본문 = index.html" "내용이 다름"
    fi
    expect_ms "P2-S3 C 는 A 가 끊긴 뒤 (약 ${HEADER_MS}ms)" "$HEADER_MS" "$EC"
    expect "P2-S3 A1 받은 바이트 0 (무응답 close)" 0 "$(tr -d ' ' < "$WORK/a1")"
    expect "P2-S3 A2 받은 바이트 0 (무응답 close)" 0 "$(tr -d ' ' < "$WORK/a2")"

    for _ in $(seq 20); do
        [ "$(ls "/proc/$SRV3/fd" 2>/dev/null | wc -l | tr -d ' ')" = "$S3FD0" ] && break
        sleep 0.1
    done
    expect "P2-S3 서버 fd 수 그대로"              "$S3FD0" "$(ls "/proc/$SRV3/fd" 2>/dev/null | wc -l | tr -d ' ')"

    # keep-alive idle 중 SIGTERM: idle 시간을 다 기다리지 않고 바로 닫고 종료
    timeout 20 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$P3"'
printf "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n" >&3
cat <&3' > "$WORK/idle-stop" &
    IDL=$!
    sleep_ms $(( KEEPALIVE_MS / 5 ))        # 응답을 받고 idle 대기에 들어갈 시간
    T0=$(now_ms)
    kill -TERM "$SRV3"
    wait "$SRV3"
    RC=$?
    EL=$(( $(now_ms) - T0 ))
    SRV3=
    wait "$IDL"
    expect "P2-S3 서버 SIGTERM → exit 0"          0 "$RC"
    expect "idle 중 SIGTERM: 응답 200 은 받음"    1 "$(grep -c '^HTTP/1.1 200 OK' "$WORK/idle-stop")"
    if grep -qF "(keep-alive closed: shutdown, 1 requests" "$LOG3"; then
        ok "idle 중 SIGTERM → 로그 (keep-alive closed: shutdown"
    else
        fail "idle 중 SIGTERM → 로그 (keep-alive closed: shutdown" "로그에 없음"
    fi
    expect_between "idle 중 SIGTERM → idle 시간 전에 종료 (ms)" 0 $(( KEEPALIVE_MS - 1 )) "$EL"
    expect "P2-S3 서버 sanitizer 보고 없음"       0 "$(grep -cE 'Sanitizer|runtime error' "$LOG3")"

    # P2-S5: 응답 전송 중 SIGINT → 새 연결은 거부, 전송 중인 응답은 끝까지, 그 뒤 exit 0
    P5=$((PORT + 2))
    LOG5=$WORK/server5.log
    start_extra "$P5" "$LOG5"
    S5=$XPID
    # B: mid.bin 을 요청하고 잠깐 읽지 않다가(서버가 전송 중에 멈춰 있게) 끝까지 읽음
    timeout 20 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$P5"'
printf "GET /mid.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" >&3
sleep '"$(ms_to_s $(( SHUTDOWN_MS / 5 )))"'
cat <&3' > "$WORK/s5.b" &
    B=$!
    sleep_ms $(( SHUTDOWN_MS / 10 ))
    kill -INT "$S5"
    sleep 0.1                               # listen 소켓이 닫힐 시간
    curl -s -o /dev/null --max-time 5 "http://127.0.0.1:$P5/index.html"
    DRC=$?
    wait "$B"
    wait "$S5"
    RC=$?
    expect "P2-S5 종료 중 새 연결 D → 거부 (curl exit 7)" 7 "$DRC"
    expect "P2-S5 B → 200"                        "HTTP/1.1 200 OK" "$(head -1 "$WORK/s5.b" | tr -d '\r')"
    if tail -c 10485760 "$WORK/s5.b" | cmp -s - "$R/mid.bin"; then
        ok "P2-S5 B 본문 10MB 끝까지 동일"
    else
        fail "P2-S5 B 본문 10MB 끝까지 동일" "받은 바이트 $(wc -c < "$WORK/s5.b")"
    fi
    expect "P2-S5 서버 exit 0 (정상 종료)"        0 "$RC"
    if grep -qF "shutting down: waiting for 1 connection(s)" "$LOG5"; then
        ok "P2-S5 로그 waiting for 1 connection(s)"
    else
        fail "P2-S5 로그 waiting for 1 connection(s)" "로그에 없음 (B 가 이미 다 받았을 수 있음)"
    fi
    expect "P2-S5 서버 sanitizer 보고 없음"       0 "$(grep -cE 'Sanitizer|runtime error' "$LOG5")"

    # 강제 종료: 응답을 안 읽는 클라이언트가 있으면 약 SHUTDOWN_MS 뒤 끊고 exit 2
    # stall 포트 : big.bin 을 요청하고 읽지 않은 채 연결만 잡고 있음
    stall() {
        timeout 20 bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$1"'
printf "GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n" >&3
sleep 15' &
        STALL=$!
    }
    P6=$((PORT + 3))
    LOG6=$WORK/server6.log
    start_extra "$P6" "$LOG6"
    S6=$XPID
    stall "$P6"
    sleep 0.3                               # 서버가 전송하다 소켓 버퍼가 찰 시간
    T0=$(now_ms)
    kill -INT "$S6"
    wait "$S6"
    RC=$?
    EL=$(( $(now_ms) - T0 ))
    kill "$STALL" 2>/dev/null
    wait "$STALL" 2>/dev/null
    expect "강제 종료(시간 초과) → exit 2"        2 "$RC"
    expect_ms "강제 종료까지 약 ${SHUTDOWN_MS}ms" "$SHUTDOWN_MS" "$EL"
    if grep -qF "forced close of 1 connection(s)" "$LOG6"; then
        ok "강제 종료 로그 forced close of 1 connection(s)"
    else
        fail "강제 종료 로그 forced close of 1 connection(s)" "로그에 없음"
    fi
    expect "강제 종료 서버 sanitizer 보고 없음"   0 "$(grep -cE 'Sanitizer|runtime error' "$LOG6")"

    # 두 번째 SIGINT: 남은 대기를 건너뛰고 바로 강제 종료
    P7=$((PORT + 4))
    LOG7=$WORK/server7.log
    start_extra "$P7" "$LOG7"
    S7=$XPID
    stall "$P7"
    sleep 0.3
    T0=$(now_ms)
    kill -INT "$S7"
    sleep 0.2
    kill -INT "$S7"
    wait "$S7"
    RC=$?
    EL=$(( $(now_ms) - T0 ))
    kill "$STALL" 2>/dev/null
    wait "$STALL" 2>/dev/null
    expect "두 번째 SIGINT → exit 2"              2 "$RC"
    expect_between "두 번째 SIGINT → 대기 시간 전에 종료 (ms)" 0 $(( SHUTDOWN_MS * 9 / 10 )) "$EL"
    expect "두 번째 SIGINT 서버 sanitizer 보고 없음" 0 "$(grep -cE 'Sanitizer|runtime error' "$LOG7")"
fi

# ---------------------------------------------------------------- 리버스 프록시 (phase 3)

echo "== 리버스 프록시"
# 백엔드 A 와 프록시를 따로 띄운다. 백엔드 파일 본문에 자기 포트를 넣어 누가 응답했는지 확인.
PB=$((PORT + 10))      # 백엔드 A
PP=$((PORT + 11))      # 프록시
PD=$((PORT + 12))      # 아무도 listen 하지 않는 포트 (connect 거절)
PX=$((PORT + 13))      # 자기 자신 규칙 거부 확인용 (실제로 listen 하지 않음)
PB2=$((PORT + 14))     # 백엔드 B (라운드로빈)
PD2=$((PORT + 15))     # 또 하나의 꺼진 포트 (모든 백엔드 실패)
B=$WORK/backend
B2=$WORK/backend2
mkdir -p "$B/api" "$B/rr" "$B2/rr" "$B/half"
echo "backend $PB" > "$B/half/hello.txt"
echo '<h1>backend</h1>' > "$B/index.html"
echo '<h1>backend2</h1>' > "$B2/index.html"
echo "backend $PB" > "$B/api/hello.txt"
echo "backend $PB" > "$B/rr/hello.txt"
echo "backend $PB2" > "$B2/rr/hello.txt"
cp "$R/mid.bin" "$B/api/mid.bin"
LOGB=$WORK/backend.log
LOGB2=$WORK/backend2.log
LOGP=$WORK/proxy.log
start_extra "$PB" "$LOGB" -r "$B"
SB=$XPID
start_extra "$PB2" "$LOGB2" -r "$B2"
SB2=$XPID
start_extra "$PP" "$LOGP" -R "/api/=127.0.0.1:$PB" -R "/down/=127.0.0.1:$PD" \
            -R "/rr/=127.0.0.1:$PB,127.0.0.1:$PB2" \
            -R "/half/=127.0.0.1:$PD,127.0.0.1:$PB" \
            -R "/alldown/=127.0.0.1:$PD,127.0.0.1:$PD2"
SP=$XPID
PURL=http://127.0.0.1:$PP

pfd() { ls "/proc/$SP/fd" 2>/dev/null | wc -l | tr -d ' '; }
pfd_settled() {
    local prev cur
    prev=$(pfd)
    for _ in $(seq 20); do
        sleep 0.1
        cur=$(pfd)
        [ "$cur" = "$prev" ] && break
        prev=$cur
    done
    echo "$cur"
}
# wait_log 파일 문자열 : 서버가 응답 뒤에 로그를 쓰므로 잠깐 기다리며 확인
wait_log() {
    for _ in $(seq 20); do grep -qF -- "$2" "$1" && return 0; sleep 0.05; done
    return 1
}
PFD0=$(pfd_settled)

# P1: /api/ 규칙 → 백엔드 A 가 응답
H=$(curl -s -D - -o "$WORK/p1.body" "$PURL/api/hello.txt" | tr -d '\r')
expect_match "P1 200"                              '^HTTP/1.1 200 OK$' "$H"
expect "P1 본문 = 백엔드 A 의 파일"                  "backend $PB" "$(cat "$WORK/p1.body")"
expect_match "P1 Content-Length = 받은 본문 길이"   "^Content-Length: $(wc -c < "$WORK/p1.body" | tr -d ' ')\$" "$H"
expect_match "P1 Content-Type 은 백엔드 값 그대로"   '^Content-Type: text/plain; charset=utf-8$' "$H"
expect "P1 Connection 헤더 없음 (연결 유지)"        "" "$(printf '%s\n' "$H" | grep -i '^Connection:')"
if wait_log "$LOGB" "\"GET /api/hello.txt HTTP/1.0\" 200"; then
    ok "P1 백엔드는 HTTP/1.0 요청으로 받음 (경로 그대로)"
else
    fail "P1 백엔드는 HTTP/1.0 요청으로 받음 (경로 그대로)" "백엔드 로그에 없음"
fi
if wait_log "$LOGP" "\"GET /api/hello.txt HTTP/1.1\" 200 $(wc -c < "$WORK/p1.body" | tr -d ' ') (proxy 127.0.0.1:$PB)"; then
    ok "P1 프록시 로그 (proxy 127.0.0.1:$PB)"
else
    fail "P1 프록시 로그 (proxy 127.0.0.1:$PB)" "프록시 로그에 없음"
fi

curl -s -o /dev/null "$PURL/api/hello.txt?x=1&y=%20"
if wait_log "$LOGB" "\"GET /api/hello.txt?x=1&y=%20 HTTP/1.0\" 200"; then
    ok "쿼리 스트링도 원문 그대로 전달"
else
    fail "쿼리 스트링도 원문 그대로 전달" "백엔드 로그에 없음"
fi

expect "Content-Length 가 있으면 클라이언트 연결 유지 (두 요청 한 연결)" "200:1 200:0 " \
       "$(curl -s -o /dev/null -o /dev/null -w '%{http_code}:%{num_connects} ' \
              "$PURL/api/hello.txt" "$PURL/api/hello.txt")"
expect "Connection: close 요청이면 응답도 close" "Connection: close" \
       "$(curl -s -D - -o /dev/null -H 'Connection: close' "$PURL/api/hello.txt" | tr -d '\r' | grep -i '^Connection:')"
expect "백엔드 404 는 그대로 전달"                   404 "$(code "$PURL/api/none.txt")"
curl -s -o "$WORK/p1.mid" "$PURL/api/mid.bin"
if cmp -s "$WORK/p1.mid" "$R/mid.bin"; then
    ok "10MB 본문 중계 (cmp 일치)"
else
    fail "10MB 본문 중계 (cmp 일치)" "내용이 다름 ($(wc -c < "$WORK/p1.mid") 바이트)"
fi
expect "규칙에 없는 경로는 프록시 자신이 처리 (P7 결정 전 현재 동작)" '<h1>hi</h1>' \
       "$(curl -s "$PURL/index.html")"
expect "POST 는 405 (본문 전달은 P8)"               405 "$(code -X POST -d 'a=1' "$PURL/api/hello.txt")"

# 꺼진 백엔드: connect 가 바로 거절 → 502
expect "꺼진 백엔드 → 502"                          502 "$(code "$PURL/down/x")"
if wait_log "$LOGP" "(proxy 127.0.0.1:$PD, connect: Connection refused)"; then
    ok "502 로그 (connect: Connection refused)"
else
    fail "502 로그 (connect: Connection refused)" "프록시 로그에 없음"
fi
expect "502 뒤에도 프록시 정상"                     200 "$(code "$PURL/api/hello.txt")"

# P2: /rr/ → A, B 라운드로빈. 따로 4번 → A B A B (규칙마다 카운터가 따로라 /api/ 요청과 무관)
RR=
for _ in 1 2 3 4; do
    RR="$RR$(curl -s "$PURL/rr/hello.txt" | sed 's/^backend //') "
done
expect "P2 라운드로빈 4번 → $PB $PB2 $PB $PB2" "$PB $PB2 $PB $PB2 " "$RR"
# 한 연결(keep-alive) 안에서도 요청마다 다음 백엔드
expect "P2 한 연결의 요청 두 개도 차례대로" "backend $PB
backend $PB2" "$(curl -s "$PURL/rr/hello.txt" "$PURL/rr/hello.txt")"
# 동시 요청 40개 (curl 4개 × 10개씩 병렬) → 정확히 20 : 20
RRP=()
for i in 1 2 3 4; do
    curl -s $(for _ in $(seq 10); do printf '%s ' "$PURL/rr/hello.txt"; done) > "$WORK/rr$i.out" &
    RRP+=("$!")
done
wait "${RRP[@]}"                            # 인자 없는 wait 는 띄워 둔 서버까지 기다림
cat "$WORK"/rr[1-4].out > "$WORK/rr.out"
expect "P2 동시 40개 → A 20개" 20 "$(grep -c "^backend $PB\$" "$WORK/rr.out")"
expect "P2 동시 40개 → B 20개" 20 "$(grep -c "^backend $PB2\$" "$WORK/rr.out")"

# P3: /half/ = [꺼진 포트, A]. 꺼진 쪽이 골려도 connect 실패라 A 로 재시도 → 4번 모두 200
RR=
for _ in 1 2 3 4; do
    RR="$RR$(curl -s -w ':%{http_code}' "$PURL/half/hello.txt" | tr -d '\n') "
done
expect "P3 한 대가 꺼져도 4번 모두 A 가 200" \
       "backend $PB:200 backend $PB:200 backend $PB:200 backend $PB:200 " "$RR"
if wait_log "$LOGP" "(proxy retry: 127.0.0.1:$PD connect: Connection refused, trying 127.0.0.1:$PB)"; then
    ok "P3 재시도 로그"
else
    fail "P3 재시도 로그" "프록시 로그에 없음"
fi
expect "P3 재시도는 2번 (4번 중 꺼진 쪽이 골린 2번)" 2 \
       "$(grep -cF "(proxy retry: 127.0.0.1:$PD connect" "$LOGP")"
# 모두 꺼짐 → 502
expect "P3 백엔드가 모두 꺼짐 → 502"                 502 "$(code "$PURL/alldown/x")"
if wait_log "$LOGP" "(proxy all 2 backends failed)"; then
    ok "P3 502 로그 (proxy all 2 backends failed)"
else
    fail "P3 502 로그 (proxy all 2 backends failed)" "프록시 로그에 없음"
fi

# 자기 자신을 가리키는 규칙은 시작할 때 거부
"$BIN" -p "$PX" -r "$R" -R "/a/=127.0.0.1:$PX" 2>"$WORK/self.err"
expect "자기 자신 규칙 → 시작 실패 exit 1"           1 "$?"
expect_match "자기 자신 규칙 메시지"                'points to this server' "$(cat "$WORK/self.err")"

PFD1=$(pfd_settled)
expect "프록시 fd 수 그대로 (connfd·backendfd 정리)" "$PFD0" "$PFD1"

kill -TERM "$SP" "$SB" "$SB2"
wait "$SP"; RCP=$?
wait "$SB"; RCB=$?
wait "$SB2"; RCB2=$?
expect "프록시 SIGTERM → exit 0"                     0 "$RCP"
expect "백엔드 A·B SIGTERM → exit 0"                 "0 0" "$RCB $RCB2"
expect "프록시·백엔드 sanitizer 보고 없음"            0 \
       "$(cat "$LOGP" "$LOGB" "$LOGB2" | grep -cE 'Sanitizer|runtime error')"

# ---------------------------------------------------------------- fd 누수

# 위 시나리오(에러, CGI, 끊김, 시간 초과 등)를 모두 거친 뒤에도 시작 때와 정확히 같아야 한다.
echo "== fd 누수"
for _ in $(seq 20); do
    [ "$(fd_count)" = "$FD0" ] && break
    sleep 0.1
done
FD1=$(fd_count)
if [ "$FD1" = "$FD0" ]; then
    ok "서버 fd 수 그대로 (시작 $FD0, 끝 $FD1)"
else
    fail "서버 fd 수 그대로" "시작 $FD0, 끝 $FD1"
    ls -l "/proc/$SRV/fd" | sed 's/^/    /'
fi

# ---------------------------------------------------------------- 종료

echo "== 종료"
if [ "$TIMING" = 1 ]; then
    # 처리 중인 요청이 있을 때 SIGINT: 그 요청을 끝까지 처리한 뒤 exit 0
    raw_status 'GET / HTTP/1.1\r\nHo' > "$WORK/inflight" &
    CLI=$!
    sleep_ms $(( HEADER_MS / 3 ))          # 서버가 그 요청의 헤더를 기다리는 도중에
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
