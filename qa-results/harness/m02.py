#!/usr/bin/env python3
"""M02: 일반 빌드에서 정상 GET 100 + 응답 중단 100 을 3회, 유휴 시 FD·자식 수 측정."""
import json
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

QA = Path("/workspace/qa")
CAND = sys.argv[1]
LOG = QA / "logs" / CAND
sys.argv = [sys.argv[0], CAND]
exec(compile(open(QA / "func_lib.py").read(), "func_lib", "exec"))  # Conn, get, start 등 재사용

out = open(LOG / "m02.jsonl", "w")


def children(pid):
    kids = set()
    for t in Path(f"/proc/{pid}/task").iterdir():
        try:
            kids.update(int(x) for x in (t / "children").read_text().split())
        except OSError:
            pass
    return sorted(kids)


def snapshot(pid):
    fds = len(list(Path(f"/proc/{pid}/fd").iterdir()))
    kids = children(pid)
    return {"pid": pid, "fds": fds, "children": [
        {"pid": k, "fds": len(list(Path(f"/proc/{k}/fd").iterdir())), "children": children(k)} for k in kids]}


def idle_snapshot(pid):
    time.sleep(1.5)  # 같은 유휴 조건: 마지막 요청 뒤 1.5초
    return snapshot(pid)


def abort(port, path):
    try:
        c = socket.create_connection(("127.0.0.1", port), timeout=5)
        c.sendall(get_raw(path))
        c.recv(65536)
        c.close()
        return True
    except OSError:
        return False


start_backend("A", PA)
start_backend("B", PB)
start_server("direct")
start_server("proxy")
for name, port, ok_path, big in (("direct", DIRECT, "/small.html", "/big.png"),
                                 ("proxy", PROXY, "/a/small", "/a/big")):
    pid = PROCS[name].pid
    for _ in range(20):
        get(port, ok_path)
    base = idle_snapshot(pid)
    rows = [{"phase": "예열 직후", **base}]
    for rnd in (1, 2, 3):
        good = sum(get(port, ok_path).get("status") == 200 for _ in range(100))
        aborted = sum(abort(port, big) for _ in range(100))
        extra = {}
        if name == "proxy":
            stop("backend-b")
            extra["b_down_status"] = get(port, "/b/ok", timeout=7).get("status")
            start_backend("B", PB)
            for _ in range(50):
                if get(port, "/b/ok", timeout=3).get("status") == 200:
                    extra["b_recovered"] = True
                    break
                time.sleep(0.1)
        rows.append({"phase": f"{rnd}회 묶음 뒤", "ok_get": good, "aborted": aborted, **extra,
                     **idle_snapshot(pid)})
    for r in rows:
        out.write(json.dumps({"server": name, **r}, ensure_ascii=False) + "\n")
        print(name, json.dumps(r, ensure_ascii=False), flush=True)
    fds = [r["fds"] for r in rows]
    stable = max(fds) - fds[0] <= 0
    rec("M02", f"{name} FD 안정 (예열 {fds[0]} → {fds[1:]})", "통과" if stable else "관찰필요", fds=fds,
        children=[len(r["children"]) for r in rows])
for n in list(PROCS):
    pid = PROCS[n].pid
    code = stop(n)
    left = [k for k in children(pid)] if Path(f"/proc/{pid}").exists() else []
    print(f"stop {n} pid={pid} exit={code} leftover_children={left}", flush=True)
