QA=/workspace/qa
for CANDIDATE in eun woong ldc; do
  cd /workspace/$CANDIDATE; LOG=$QA/logs/$CANDIDATE
  unset MAKEFLAGS MFLAGS CC CFLAGS CPPFLAGS LDFLAGS LDLIBS
  case "$CANDIDATE" in
    eun)   make -B all CFLAGS='-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2 -g' ;;
    woong) make -B all CFLAGS='-std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -g' ;;
    ldc)   make -B all CFLAGS='-std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -g -O2 -MMD -MP -pthread' ;;
  esac > $LOG/build-load.log 2>&1
  printf 'load build exit=%s\n' "$?" | tee -a $LOG/status.log | sed "s/^/$CANDIDATE /"
done
cd /workspace/eun
make -B all CFLAGS='-std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -O2 -g' > $QA/logs/eun/build-load-diag.log 2>&1
printf 'load diag build exit=%s (추가 옵션 -D_XOPEN_SOURCE=700)\n' "$?" | tee -a $QA/logs/eun/status.log | sed 's/^/eun /'
