QA=/workspace/qa
set -o pipefail
run_one() {
  CANDIDATE=$1 ROUND=$2; cd /workspace/$CANDIDATE; LOG=$QA/logs/$CANDIDATE
  case "$CANDIDATE" in
    eun)   direct=(./server -p 20801 -r "$QA/one"); proxy=(./server -p 20802 -r "$QA/one" -R /a/=127.0.0.1:20803 -R /b/=127.0.0.1:20804); tag=load-diag ;;
    woong) direct=(./webserver --port 20801 --root "$QA/one"); proxy=(./reverse-proxy --port 20802 --route /a/=127.0.0.1:20803 --route /b/=127.0.0.1:20804); tag=load ;;
    ldc)   direct=(./server 20801 "$QA/one"); proxy=(./server --config "$QA/proxy.conf"); tag=load ;;
  esac
  python3 $QA/backend.py A 20803 --quiet > $LOG/load-backend.log 2>&1 & A=$!
  python3 $QA/backend.py B 20804 --quiet >> $LOG/load-backend.log 2>&1 & B=$!
  "${direct[@]}" > $LOG/load-direct-$ROUND.log 2>&1 & S=$!
  "${proxy[@]}" > $LOG/load-proxy-$ROUND.log 2>&1 & P=$!
  sleep 1.5
  python3 $QA/load.py > $LOG/$tag-$ROUND.jsonl; rc=$?
  echo "round $ROUND $CANDIDATE load.py exit=$rc" | tee -a $LOG/status.log
  kill -TERM $S $P $A $B; wait $S $P $A $B 2>/dev/null
  sleep 1
}
for c in eun woong ldc; do run_one $c 1; done
for c in woong ldc eun; do run_one $c 2; done
for c in ldc eun woong; do run_one $c 3; done
echo LOAD-DONE
