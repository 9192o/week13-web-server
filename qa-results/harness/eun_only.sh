QA=/workspace/qa; cd $QA; bash setup.sh >/dev/null 2>&1
cd /workspace/eun; unset MAKEFLAGS MFLAGS CC CFLAGS CPPFLAGS LDFLAGS LDLIBS; L=$QA/logs/eun; mkdir -p $L
make all > $L/build.log 2>&1; echo "F01 build exit=$? warn=$(grep -c warning $L/build.log)"
make test > $L/unit.log 2>&1; echo "unit exit=$? fails=$(grep -c ' [1-9][0-9]* failures' $L/unit.log)"
python3 $QA/func.py eun 2>&1 | grep -E "^(실패|미실행)|SUMMARY"
make -B all CFLAGS='-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2 -g' > $L/build-load.log 2>&1; echo "load build exit=$?"
sed -i 's/^for c in .*run_one $c 1; done/run_one eun 1/; s/^for c in .*run_one $c 2; done/run_one eun 2/; s/^for c in .*run_one $c 3; done/run_one eun 3/; s/tag=load-diag/tag=load/' $QA/loadrun.sh
bash $QA/loadrun.sh > /dev/null 2>&1
python3 -c '
import json,glob,statistics as st
r={}
for f in glob.glob("/workspace/qa/logs/eun/load-[123].jsonl"):
  for l in open(f):
    d=json.loads(l);m=d["measurement"];r.setdefault((d["work"],d["concurrency"]),[]).append((m["success_rps"],m["p95_ms"],m["error_rate"]))
for k in sorted(r): v=r[k];print(k[0],k[1],"rps=%.0f p95=%.2f maxerr=%.3f"%(st.median(x[0] for x in v),st.median(x[1] for x in v),max(x[2] for x in v)))'
sed -i 's/^for CANDIDATE in eun woong ldc; do/for CANDIDATE in eun; do/' $QA/mem.sh
bash $QA/mem.sh 2>&1 | grep -E "exit=[^0]|실패|SUMMARY|lost: [1-9]"
python3 $QA/m02.py eun 2>&1 | grep -E "M02|leftover"
echo ALL-DONE
