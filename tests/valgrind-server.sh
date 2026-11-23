#!/bin/bash
#
# ./server 를 valgrind 아래에서 실행하는 래퍼. functional.sh 에 서버 대신 넘긴다.
#   make valgrind  →  bash tests/functional.sh tests/valgrind-server.sh
#
# 메모리 오류나 확실한 누수(definite/indirect)가 있으면 서버 종료 코드가 99 가 되어
# functional.sh 의 "SIGTERM → exit 0" 검사가 실패한다. valgrind 보고는 서버 stderr
# (= functional.sh 의 서버 로그)로 나간다.
# CGI 용으로 fork 한 자식의 보고는 끄고, exec 한 CGI 프로그램은 추적하지 않는다.

REPO=$(cd "$(dirname "$0")/.." && pwd)

exec valgrind --quiet \
    --leak-check=full \
    --errors-for-leak-kinds=definite,indirect \
    --error-exitcode=99 \
    --child-silent-after-fork=yes \
    --trace-children=no \
    "$REPO/server" "$@"
