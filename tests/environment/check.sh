#!/bin/sh
set -eu

test "$(uname -s)" = Linux
test "$(uname -m)" = x86_64
if [ "$(id -u)" -eq 0 ]; then
    printf 'Run the development container as a non-root user.\n' >&2
    exit 1
fi

for tool in gcc make python3 curl gdb strace valgrind; do
    command -v "$tool"
done

uname -srmo
dpkg-query -W -f='${Package}=${Version}\n' \
    gcc libc6-dev make python3 curl gdb strace valgrind
gcc --version
python3 --version

temp_dir=$(mktemp -d)
workspace_file=
trap 'rm -rf "$temp_dir"; if [ -n "$workspace_file" ]; then rm -f "$workspace_file"; fi' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

workspace_file=$(mktemp "$PWD/.dev-env-check.XXXXXX")
printf 'Workspace is writable as uid=%s gid=%s.\n' "$(id -u)" "$(id -g)"

gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g \
    tests/environment/smoke.c -o "$temp_dir/smoke"
"$temp_dir/smoke"
printf 'C17 and POSIX compile/run: PASS\n'

gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    tests/environment/smoke.c -o "$temp_dir/sanitized"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 "$temp_dir/sanitized"
printf 'ASan, UBSan and leak-check execution: PASS\n'

valgrind --quiet --leak-check=full --errors-for-leak-kinds=all \
    --error-exitcode=99 "$temp_dir/smoke"
printf 'Valgrind execution: PASS\n'
printf 'DEV_ENV_OK\n'
