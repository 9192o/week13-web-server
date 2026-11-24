#!/bin/bash
#
# 기능 테스트: 서버를 임시 문서 루트로 띄우고 curl / raw 요청으로 시나리오를 확인한다.
#
#   사용법: tests/functional.sh <서버 바이너리> [--timing]
#     --timing  시간 제한 시나리오(408, 무응답 close, CGI 시간 초과, 전송 시간 초과,
#               처리 중 SIGINT)까지 실행
#     PORT      환경 변수로 포트 변경 (기본 18099)
#     HEADER_MS BODY_MS CGI_MS SEND_MS
#               서버가 빌드된 시간 제한 값(ms). 기본은 실제 값 5000/10000/5000/30000.
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
fi

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
