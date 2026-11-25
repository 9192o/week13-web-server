export QA=/workspace/qa
set -o pipefail
for CANDIDATE in eun woong ldc; do
  cd "/workspace/$CANDIDATE"
  unset MAKEFLAGS MFLAGS CC CFLAGS CPPFLAGS LDFLAGS LDLIBS
  LOG="$QA/logs/$CANDIDATE"; mkdir -p "$LOG"
  make all > "$LOG/build.log" 2>&1
  printf 'build exit=%s\n' "$?" > "$LOG/status.log"
  case "$CANDIDATE" in
    woong) unit=(test-unit test-proxy-unit) ;;
    eun|ldc) unit=(test) ;;
  esac
  timeout 900 make "${unit[@]}" > "$LOG/unit.log" 2>&1
  printf 'unit exit=%s\n' "$?" >> "$LOG/status.log"
  echo "===== $CANDIDATE"; cat "$LOG/status.log"
  grep -ciE "warning" "$LOG/build.log" | sed 's/^/build warnings: /'
  grep -E "FAIL|checks|failed|passed|assertions|Ran |OK$|Error" "$LOG/unit.log" | tail -25
done
