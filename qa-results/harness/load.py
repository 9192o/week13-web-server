from concurrent.futures import ThreadPoolExecutor
from http.client import HTTPConnection
import json
import time

def get(port, path, verify_body=False):
    start = time.monotonic()
    conn = HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.request("GET", path, headers={"Connection": "close"})
        with conn.getresponse() as response:
            data = response.read()
            if response.status != 200 or len(data) != 1024:
                raise ValueError(f"expected 200/1024, got {response.status}/{len(data)}")
            if verify_body and data != b"x" * 1024:
                raise ValueError("fixture bytes differ")
        return (time.monotonic() - start) * 1000, None
    except Exception as exc:
        return None, f"{type(exc).__name__}: {exc}"
    finally:
        conn.close()

def run(port, path, workers, count):
    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(lambda _: get(port, path), range(count)))
    elapsed = time.monotonic() - start
    good = sorted(ms for ms, error in results if error is None)
    quantile = lambda q: good[int((len(good) - 1) * q)] if good else None
    return {"count": count, "success": len(good), "seconds": elapsed,
            "success_rps": len(good) / elapsed, "p50_ms": quantile(0.5),
            "p95_ms": quantile(0.95), "error_rate": 1 - len(good) / count,
            "errors": sorted({error for _, error in results if error is not None})[:5]}

for work, port, path in (("L01", 20801, "/small.html"), ("L02", 20802, "/a/small"),
                         ("backend", 20803, "/a/small")):
    for workers in (1, 8, 32):
        _, before = get(port, path, verify_body=True)
        if before:
            raise SystemExit(f"{work}: precheck failed: {before}")
        warm = run(port, path, workers, 100)
        measured = run(port, path, workers, 2000)
        _, after = get(port, path, verify_body=True)
        print(json.dumps({"work": work, "concurrency": workers, "warmup": warm,
                          "measurement": measured, "postcheck_error": after}), flush=True)
        if after:
            raise SystemExit(f"{work}: postcheck failed: {after}")
