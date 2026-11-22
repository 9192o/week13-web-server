#!/bin/sh
# Raw HTTP test helper.
# Each typed line is sent with CRLF, so the server's \r\n\r\n parser sees it.
#
# usage: sh tests/raw.sh [port] [host]
# example:
#   sh tests/raw.sh 8080
#   GET / HTTP/1.1
#   Host: localhost
#   Connection: close
#   (blank line)
set -u

PORT=${1:-8080}
HOST=${2:-127.0.0.1}

printf 'raw HTTP -> %s:%s\n' "$HOST" "$PORT" >&2
printf 'type request lines; a blank line ends the headers; Ctrl-C to quit\n' >&2

while IFS= read -r line; do
    printf '%s\r\n' "$line"
done | nc -w 30 "$HOST" "$PORT"
