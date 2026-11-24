#!/bin/bash
#
# 기준선 부하 테스트: wrk 로 정적 파일(www/index.html) 처리량과 지연을 잰다.
#
#   사용법: bench/run.sh [결과 디렉터리]   (기본 bench/results/<날짜-시각>)
#     문서 루트는 www/ 를 /tmp 로 복사해서 쓴다 (WSL /mnt 파일 접근이 병목이 되지 않게)
#     REPS  조합마다 반복 횟수 (기본 3, 표에는 2xx 처리량의 중앙값 회차를 씀)
#     DUR   wrk 한 번의 측정 시간(초) (기본 10)
#     PORT  서버 포트 (기본 18199)
#     ONLY  phase2 | q1024 | phase1 | logcost 중 하나만 (기본 전부)
#
# 매트릭스
#   phase2  : 동시 10/100/1000 x 워커 8/64 x keep-alive 유/무, 큐 64 (기본)
#   q1024   : 동시 1000 x 워커 8/64 x keep-alive 유/무, 큐 1024
#   phase1  : 순차 처리 커밋(cc05337), 동시 10/100/1000, keep-alive 없음
#   logcost : phase2 워커 8, 동시 100, keep-alive, 접근 로그를 파일로 (나머지는 /dev/null)
#
# 결과: raw/*.txt (wrk 원본), runs.csv (회차별), summary.csv (중앙값 회차),
#       logcost-*.txt (로그 비용 조합의 상태 코드·keep-alive 닫힘 이유 집계)
# 필요 도구: wrk, curl, git, make, cc

set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$REPO/bench/results/$(date +%Y%m%d-%H%M%S)}
REPS=${REPS:-3}
DUR=${DUR:-10}
PORT=${PORT:-18199}
ONLY=${ONLY:-}
THREADS=4                          # wrk 스레드: 8코어의 절반 (나머지는 서버 몫)
PHASE1_COMMIT=cc05337
URL=http://127.0.0.1:$PORT/index.html

command -v wrk >/dev/null || { echo "wrk 가 없음 (sudo apt install wrk)" >&2; exit 1; }
mkdir -p "$OUT/raw"
TMP=$(mktemp -d /tmp/ws-bench-XXXXXX)

# 문서 루트는 리눅스 파일시스템(/tmp)에 복사해서 쓴다. WSL 에서 레포가 /mnt/<드라이브> 에
# 있으면 파일 접근(realpath/open/fstat)이 9P 를 거쳐 요청마다 ms 단위가 되고,
# 처리량이 서버와 상관없이 ~3000 req/s 에서 막힌다 (실측: 같은 조건 2.9k → 53k req/s).
ROOT=$TMP/www
cp -r "$REPO/www" "$ROOT"
SRV=
cleanup() {
    if [ -n "$SRV" ] && kill -0 "$SRV" 2>/dev/null; then
        kill -KILL "$SRV" 2>/dev/null
        wait "$SRV" 2>/dev/null
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

# ---------------------------------------------------------------- 빌드

echo "== build"
make -C "$REPO" all >/dev/null || exit 1
P2BIN=$REPO/server
P1BIN=
if [ -z "$ONLY" ] || [ "$ONLY" = phase1 ]; then
    mkdir -p "$TMP/phase1"
    git -C "$REPO" archive "$PHASE1_COMMIT" | tar -x -C "$TMP/phase1" || exit 1
    make -C "$TMP/phase1" server >/dev/null || exit 1
    P1BIN=$TMP/phase1/server
fi

# ---------------------------------------------------------------- 환경 기록

{
    echo "date: $(date -Is)"
    echo "kernel: $(uname -r)"
    echo "cpu: $(lscpu | sed -n 's/^Model name: *//p') x $(nproc)"
    echo "wrk: $(wrk -v 2>&1 | head -1)"
    echo "ulimit -n: $(ulimit -n)"
    echo "ip_local_port_range: $(sysctl -n net.ipv4.ip_local_port_range | tr '\t' ' ')"
    echo "tcp_tw_reuse: $(sysctl -n net.ipv4.tcp_tw_reuse)"
    echo "commit: $(git -C "$REPO" rev-parse --short HEAD)"
    echo "reps: $REPS, duration: ${DUR}s, wrk threads: $THREADS"
    echo "file: index.html $(wc -c < "$ROOT/index.html") bytes (root copied to $(df -T "$ROOT" | awk 'NR==2 {print $2}') at /tmp)"
} > "$OUT/env.txt"
cat "$OUT/env.txt"

# ---------------------------------------------------------------- 측정 도우미

# 지연 문자열(1.23us / 4.5ms / 1.2s)을 ms 로
to_ms() {
    awk -v v="$1" 'BEGIN {
        n = v + 0
        if (v ~ /us$/) n /= 1000
        else if (v ~ /ms$/) n = n
        else if (v ~ /m$/) n *= 60000
        else if (v ~ /s$/) n *= 1000
        printf "%.3f", n
    }'
}

# wrk 출력 파일 → "총요청,non2xx,초,connect,read,write,timeout,p50,p99"
parse_wrk() {
    local f=$1 total secs non2xx sc sr sw st p50 p99
    # "  123456 requests in 10.00s, 45.67MB read"
    total=$(awk '/ requests in / {print $1}' "$f")
    secs=$(awk '/ requests in / {s = $4; sub(/s,?$/, "", s); print s}' "$f")
    non2xx=$(sed -n 's/^ *Non-2xx or 3xx responses: *\([0-9]*\).*/\1/p' "$f")
    read -r sc sr sw st < <(sed -n 's/^ *Socket errors: connect \([0-9]*\), read \([0-9]*\), write \([0-9]*\), timeout \([0-9]*\).*/\1 \2 \3 \4/p' "$f")
    p50=$(awk '$1 == "50%" {print $2}' "$f")
    p99=$(awk '$1 == "99%" {print $2}' "$f")
    echo "${total:-0},${non2xx:-0},${secs:-$DUR},${sc:-0},${sr:-0},${sw:-0},${st:-0},$(to_ms "${p50:-0ms}"),$(to_ms "${p99:-0ms}")"
}

# start_server 바이너리 로그 [인자...] : 띄우고 응답할 때까지 기다림
start_server() {
    local bin=$1 log=$2
    shift 2
    "$bin" -p "$PORT" -r "$ROOT" "$@" 2>"$log" &
    SRV=$!
    for _ in $(seq 50); do
        [ "$(curl -s -o /dev/null -w '%{http_code}' "$URL")" = 200 ] && return 0
        sleep 0.1
    done
    echo "서버가 시작되지 않음: $bin $*" >&2
    exit 1
}

stop_server() {
    kill -TERM "$SRV" 2>/dev/null
    wait "$SRV" 2>/dev/null
    SRV=
}

echo "set,impl,workers,queue,conns,keepalive,log,rep,requests,non2xx,secs,sock_connect,sock_read,sock_write,sock_timeout,p50_ms,p99_ms,rps_total,rps_2xx" > "$OUT/runs.csv"

# run_case 세트 구현 워커 큐 동시 ka(on|off) 로그(null|file)
run_case() {
    local set=$1 impl=$2 w=$3 q=$4 c=$5 ka=$6 lg=$7 rep bin args name logf hdr=()
    if [ "$impl" = phase1 ]; then
        bin=$P1BIN; args=()
    else
        bin=$P2BIN; args=(-w "$w" -q "$q")
    fi
    [ "$ka" = off ] && hdr=(-H "Connection: close")
    for rep in $(seq "$REPS"); do
        name=$set-$impl-w$w-q$q-c$c-ka$ka-$lg-r$rep
        if [ "$lg" = file ]; then logf=$TMP/$name.log; else logf=/dev/null; fi
        start_server "$bin" "$logf" "${args[@]}"
        wrk -t "$THREADS" -c "$c" -d "${DUR}s" --latency "${hdr[@]}" "$URL" > "$OUT/raw/$name.txt" 2>&1
        stop_server
        local r total non2xx secs
        r=$(parse_wrk "$OUT/raw/$name.txt")
        IFS=, read -r total non2xx secs _ <<< "$r"
        echo "$set,$impl,$w,$q,$c,$ka,$lg,$rep,$r,$(awk -v t="$total" -v s="$secs" 'BEGIN{printf "%.0f", t/s}'),$(awk -v t="$total" -v n="$non2xx" -v s="$secs" 'BEGIN{printf "%.0f", (t-n)/s}')" >> "$OUT/runs.csv"
        printf '%-48s %s\n' "$name" "$(tail -1 "$OUT/runs.csv" | awk -F, '{printf "2xx %s/s  total %s/s  non2xx %s  sockerr %s/%s/%s/%s  p99 %sms", $19, $18, $10, $12, $13, $14, $15, $17}')"
        if [ "$lg" = file ]; then
            {
                echo "== $name"
                echo "-- 상태 코드별 요청 로그 줄 수"
                # 접근 로그: ip:port "요청 라인" 상태 바이트 [비고]
                awk '{ if (match($0, /" [0-9][0-9][0-9] /)) print substr($0, RSTART + 2, 3) }' \
                    "$logf" | sort | uniq -c
                echo "-- keep-alive 닫힘 이유"
                sed -n 's/.*(keep-alive closed: \([a-z]*\),.*/\1/p' "$logf" | sort | uniq -c
                echo "-- 로그 크기: $(wc -c < "$logf") bytes, $(wc -l < "$logf") lines"
            } >> "$OUT/logcost-summary.txt"
        fi
        sleep 1
    done
}

want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

if want phase2; then
    echo "== phase2 (큐 64)"
    for c in 10 100 1000; do
        for w in 8 64; do
            for ka in on off; do
                run_case phase2 phase2 "$w" 64 "$c" "$ka" null
            done
        done
    done
fi
if want q1024; then
    echo "== phase2 (큐 1024)"
    for w in 8 64; do
        for ka in on off; do
            run_case q1024 phase2 "$w" 1024 1000 "$ka" null
        done
    done
fi
if want phase1; then
    echo "== phase1 (순차, $PHASE1_COMMIT)"
    for c in 10 100 1000; do
        run_case phase1 phase1 1 0 "$c" off null
    done
fi
if want logcost; then
    echo "== 로그 비용 (워커 8, 동시 100, keep-alive, 로그 파일)"
    run_case logcost phase2 8 64 100 on file
fi

# ---------------------------------------------------------------- 중앙값

# 같은 조합(set..log)의 회차 중 2xx 처리량이 중앙값인 회차를 고른다
{
    head -1 "$OUT/runs.csv"
    tail -n +2 "$OUT/runs.csv" | sort -t, -k1,7 -k19,19n | awk -F, -v reps="$REPS" '
        { key = $1 FS $2 FS $3 FS $4 FS $5 FS $6 FS $7
          if (key != prev) { n = 0; prev = key }
          n++
          if (n == int((reps + 1) / 2)) print }'
} > "$OUT/summary.csv"

echo "== 결과: $OUT"
column -s, -t < "$OUT/summary.csv"
