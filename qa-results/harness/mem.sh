QA=/workspace/qa
vg=(valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,indirect,possible --error-exitcode=77)
for CANDIDATE in eun woong ldc; do
  cd /workspace/$CANDIDATE; LOG=$QA/logs/$CANDIDATE
  unset MAKEFLAGS MFLAGS CC CFLAGS CPPFLAGS LDFLAGS LDLIBS
  if [ $CANDIDATE = eun ]; then F='-std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -O2 -g'; make -B all test CFLAGS="$F" > $LOG/build-mem.log 2>&1
  else make -B all > $LOG/build-mem.log 2>&1; fi
  echo "$CANDIDATE mem build exit=$?" | tee -a $LOG/status.log
  case $CANDIDATE in
    eun) for t in build/tests/test_*; do "${vg[@]}" --log-file=$LOG/mem-unit-$(basename $t).log ./$t > /dev/null 2>&1; echo "$CANDIDATE unit-vg $(basename $t) exit=$?"; done ;;
    woong) make UNIT_RUNNER="${vg[*]}" test-unit test-proxy-unit > $LOG/mem-unit.log 2>&1; echo "$CANDIDATE unit-vg exit=$?" ;;
    ldc) make run_tests > /dev/null 2>&1; "${vg[@]}" --log-file=$LOG/mem-unit-run_tests.log ./run_tests > /dev/null 2>&1; echo "$CANDIDATE unit-vg run_tests exit=$?" ;;
  esac | tee -a $LOG/status.log
  python3 $QA/func.py $CANDIDATE mem 2>&1 | grep -E "^(실패|미실행|통과 M01)|SUMMARY|stop (direct|proxy)"
  grep -hE "definitely lost|indirectly lost|possibly lost|ERROR SUMMARY" $LOG/mem-direct-*.log $LOG/mem-proxy-*.log 2>/dev/null | sort | uniq -c
done
echo MEM-DONE
