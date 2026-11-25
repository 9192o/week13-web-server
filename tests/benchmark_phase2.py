#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import math
import os
import platform
import queue
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Final, List, Optional, Tuple

HOST: Final = "127.0.0.1"
READY_TIMEOUT: Final = 10.0
REQUEST_TIMEOUT: Final = 60.0
TERM_TIMEOUT: Final = 5.0

WARMUP_REQUESTS: Final = 1
SAMPLE_REQUESTS: Final = 15
DEFAULT_REPEAT: Final = 1
DEFAULT_CONCURRENCY: Final = [1, 10, 50]
DEFAULT_MAX_REQ_PER_CONN: Final = 100

STATIC_PREFIX: Final = "webserver_bench_"


def payload(size: int) -> bytes:
    pattern = bytes(range(256))
    n, r = divmod(size, 256)
    return pattern * n + pattern[:r]


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((HOST, 0))
        return int(sock.getsockname()[1])


def parse_int_list(text: str, allow_zero: bool = False) -> List[int]:
    values: List[int] = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        values.append(int(part))
    if not values:
        raise ValueError(f"empty integer list: {text!r}")
    if allow_zero:
        if any(v < 0 for v in values):
            raise ValueError(f"list values must be non-negative: {values}")
    else:
        if any(v < 1 for v in values):
            raise ValueError(f"list values must be positive: {values}")
    return values


def percentile(sorted_values: List[float], p: float) -> float:
    n = len(sorted_values)
    if n == 0:
        raise ValueError("empty values")
    idx = math.ceil(p * n) - 1
    return sorted_values[idx]


def percentiles(values: List[float]) -> Tuple[Optional[float], Optional[float], Optional[float]]:
    if not values:
        return None, None, None
    s = sorted(values)
    return (
        round(percentile(s, 0.50), 3),
        round(percentile(s, 0.95), 3),
        round(percentile(s, 0.99), 3),
    )


def build_request(
    method: str,
    path: str,
    headers: Optional[Dict[str, str]] = None,
    body: Optional[bytes] = None,
) -> bytes:
    lines = [f"{method} {path} HTTP/1.1", f"Host: {HOST}"]
    for name, value in (headers or {}).items():
        lines.append(f"{name}: {value}")
    lines.append("")
    head = "\r\n".join(lines).encode() + b"\r\n"
    if body:
        return head + body
    return head


def sha256_file(path: Path) -> Optional[str]:
    try:
        h = hashlib.sha256()
        with path.open("rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def start_server(
    executable: Path,
    port: int,
    root: Path,
    extra_args: List[str],
    log_path: Optional[Path],
) -> Tuple[subprocess.Popen[bytes], threading.Thread, queue.Queue[bytes | None], Optional[Path]]:
    cmd = [str(executable), "--port", str(port), "--root", str(root)] + extra_args
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        cwd=str(root),
    )
    stdout = proc.stdout
    assert stdout is not None

    if log_path is None:
        log_fd, log_path_str = tempfile.mkstemp(suffix=".log", prefix="webserver-bench-")
        log_file: Optional[Path] = Path(log_path_str)
        log_fh = os.fdopen(log_fd, "wb")
    elif str(log_path) == os.devnull:
        log_file = None
        log_fh = None
    else:
        log_file = log_path
        log_fh = log_path.open("wb")

    q: queue.Queue[bytes | None] = queue.Queue()

    def drain() -> None:
        try:
            with stdout:
                for line in iter(stdout.readline, b""):
                    q.put(line)
                    if log_fh is not None:
                        log_fh.write(line)
                        log_fh.flush()
        finally:
            q.put(None)
            if log_fh is not None:
                log_fh.close()

    t = threading.Thread(target=drain, daemon=True)
    t.start()

    deadline = time.monotonic() + READY_TIMEOUT
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            line = q.get(timeout=remaining)
        except queue.Empty:
            break
        if line is None:
            break
        match = re.match(rb"READY\s+(\d+)\r?$", line.strip())
        if match:
            ready_port = int(match.group(1))
            if ready_port != port:
                proc.terminate()
                try:
                    proc.wait(timeout=TERM_TIMEOUT)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=TERM_TIMEOUT)
                t.join(timeout=TERM_TIMEOUT)
                raise RuntimeError(
                    f"server ready on unexpected port {ready_port}, expected {port}; cmd={cmd}"
                )
            return proc, t, q, log_file

    proc.terminate()
    try:
        proc.wait(timeout=TERM_TIMEOUT)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=TERM_TIMEOUT)
    t.join(timeout=TERM_TIMEOUT)
    raise RuntimeError(f"server did not become ready within {READY_TIMEOUT}s; cmd={cmd}")


def stop_server(proc: subprocess.Popen[bytes], drain_thread: threading.Thread) -> None:
    if proc.poll() is not None:
        drain_thread.join(timeout=TERM_TIMEOUT)
        return
    proc.terminate()
    try:
        proc.wait(timeout=TERM_TIMEOUT)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=TERM_TIMEOUT)
    drain_thread.join(timeout=TERM_TIMEOUT)


def prepare_root(root: Path, static_sizes: List[int]) -> Dict[str, int]:
    mapping: Dict[str, int] = {}
    for size in static_sizes:
        name = f"{STATIC_PREFIX}static_{size}.png"
        path = root / name
        with path.open("xb") as f:
            f.write(payload(size))
        mapping[f"/{name}"] = size
    return mapping


def request_close(
    port: int,
    method: str,
    path: str,
    body: Optional[bytes],
    expected_body: Optional[bytes],
) -> Tuple[Optional[float], Optional[str]]:
    headers = {"Connection": "close"}
    if body is not None:
        headers["Content-Length"] = str(len(body))
    req = build_request(method, path, headers, body)
    start = time.perf_counter()
    try:
        with socket.create_connection((HOST, port), timeout=REQUEST_TIMEOUT) as sock:
            sock.sendall(req)
            response = http.client.HTTPResponse(sock)
            response.begin()
            status = response.status
            resp_body = response.read()
    except Exception as exc:
        return round((time.perf_counter() - start) * 1000, 3), f"{type(exc).__name__}: {exc}"
    elapsed = time.perf_counter() - start
    if status != 200:
        return round(elapsed * 1000, 3), f"status {status}"
    if expected_body is not None and resp_body != expected_body:
        return (
            round(elapsed * 1000, 3),
            f"body mismatch (got {len(resp_body)} bytes, expected {len(expected_body)} bytes)",
        )
    return round(elapsed * 1000, 3), None


def request_keepalive(
    conn: http.client.HTTPConnection,
    method: str,
    path: str,
    body: Optional[bytes],
    expected_body: Optional[bytes],
    force_close_after: bool,
) -> Tuple[http.client.HTTPConnection, Optional[float], Optional[str], bool]:
    headers = {"Connection": "keep-alive"}
    start = time.perf_counter()
    server_will_close = False
    try:
        conn.request(method, path, body=body, headers=headers)
        response = conn.getresponse()
        status = response.status
        resp_body = response.read()
        conn_header = response.getheader("Connection", "").lower()
        server_will_close = response.will_close or conn_header == "close"
    except Exception as exc:
        return conn, round((time.perf_counter() - start) * 1000, 3), f"{type(exc).__name__}: {exc}", True
    elapsed = time.perf_counter() - start
    if status != 200:
        return conn, round(elapsed * 1000, 3), f"status {status}", server_will_close
    if expected_body is not None and resp_body != expected_body:
        return (
            conn,
            round(elapsed * 1000, 3),
            f"body mismatch (got {len(resp_body)} bytes, expected {len(expected_body)} bytes)",
            server_will_close,
        )
    return conn, round(elapsed * 1000, 3), None, server_will_close or force_close_after


def worker_close(
    port: int,
    method: str,
    path: str,
    body: Optional[bytes],
    expected_body: Optional[bytes],
    warmup: int,
    repeat: int,
    samples: int,
) -> Dict[str, Any]:
    latencies: List[float] = []
    errors = 0
    error_samples: List[str] = []
    connections_opened = 0
    t_first: Optional[float] = None
    t_last: Optional[float] = None

    for _ in range(warmup):
        connections_opened += 1
        _latency, err = request_close(port, method, path, body, expected_body)
        if err and len(error_samples) < 10:
            error_samples.append(err)

    t_first = time.perf_counter()
    for _ in range(repeat):
        for _ in range(samples):
            connections_opened += 1
            latency, err = request_close(port, method, path, body, expected_body)
            now = time.perf_counter()
            t_last = now
            if err:
                errors += 1
                if len(error_samples) < 10:
                    error_samples.append(err)
            else:
                if latency is not None:
                    latencies.append(latency)

    return {
        "latencies": latencies,
        "errors": errors,
        "error_samples": error_samples,
        "connections_opened": connections_opened,
        "t_first": t_first,
        "t_last": t_last,
    }


def worker_keepalive(
    port: int,
    method: str,
    path: str,
    body: Optional[bytes],
    expected_body: Optional[bytes],
    warmup: int,
    repeat: int,
    samples: int,
    max_per_connection: int,
) -> Dict[str, Any]:
    latencies: List[float] = []
    errors = 0
    error_samples: List[str] = []
    connections_opened = 0
    t_first: Optional[float] = None
    t_last: Optional[float] = None

    def fresh_connection() -> http.client.HTTPConnection:
        nonlocal connections_opened
        connections_opened += 1
        return http.client.HTTPConnection(HOST, port, timeout=REQUEST_TIMEOUT)

    conn: Optional[http.client.HTTPConnection] = None
    requests_on_conn = 0

    def do_one() -> Tuple[Optional[float], Optional[str]]:
        nonlocal conn, requests_on_conn
        if conn is None:
            conn = fresh_connection()
            requests_on_conn = 0
        force_close = requests_on_conn + 1 >= max_per_connection
        conn, latency, err, will_close = request_keepalive(
            conn, method, path, body, expected_body, force_close
        )
        if will_close or force_close:
            try:
                conn.close()
            except Exception:
                pass
            conn = None
            requests_on_conn = 0
        else:
            requests_on_conn += 1
        return latency, err

    for _ in range(warmup):
        _latency, err = do_one()
        if err and len(error_samples) < 10:
            error_samples.append(err)

    t_first = time.perf_counter()
    for _ in range(repeat):
        for _ in range(samples):
            latency, err = do_one()
            now = time.perf_counter()
            t_last = now
            if err:
                errors += 1
                if len(error_samples) < 10:
                    error_samples.append(err)
            else:
                if latency is not None:
                    latencies.append(latency)

    try:
        conn.close()
    except Exception:
        pass

    return {
        "latencies": latencies,
        "errors": errors,
        "error_samples": error_samples,
        "connections_opened": connections_opened,
        "t_first": t_first,
        "t_last": t_last,
    }


def run_condition(
    label: str,
    executable: Path,
    port: int,
    root: Path,
    connection_mode: str,
    concurrency_tiers: List[int],
    workloads: List[Dict[str, Any]],
    warmup: int,
    repeat: int,
    samples: int,
    max_per_connection: int,
    server_proc: Optional[subprocess.Popen[bytes]],
) -> Tuple[List[Dict[str, Any]], subprocess.Popen[bytes], int]:
    results: List[Dict[str, Any]] = []
    total_errors = 0

    for concurrency in concurrency_tiers:
        for workload in workloads:
            method = workload["method"]
            path = workload["path"]
            body = workload.get("body")
            expected_body = workload.get("expected_body")

            def make_worker(
                method: str = method,
                path: str = path,
                body: Optional[bytes] = body,
                expected_body: Optional[bytes] = expected_body,
            ) -> Dict[str, Any]:
                if connection_mode == "close":
                    return worker_close(
                        port, method, path, body, expected_body,
                        warmup, repeat, samples,
                    )
                return worker_keepalive(
                    port, method, path, body, expected_body,
                    warmup, repeat, samples, max_per_connection,
                )

            with ThreadPoolExecutor(max_workers=concurrency) as exe:
                futures = [exe.submit(make_worker) for _ in range(concurrency)]
                worker_results = [f.result() for f in futures]

            all_latencies: List[float] = []
            errors = 0
            error_samples: List[str] = []
            connections_opened = 0
            t_first: Optional[float] = None
            t_last: Optional[float] = None
            for wr in worker_results:
                all_latencies.extend(wr["latencies"])
                errors += wr["errors"]
                error_samples.extend(wr["error_samples"])
                connections_opened += wr["connections_opened"]
                if wr["t_first"] is not None:
                    if t_first is None or wr["t_first"] < t_first:
                        t_first = wr["t_first"]
                if wr["t_last"] is not None:
                    if t_last is None or wr["t_last"] > t_last:
                        t_last = wr["t_last"]

            total_errors += errors
            elapsed = (t_last - t_first) if t_first is not None and t_last is not None else 0.0
            throughput = len(all_latencies) / elapsed if elapsed > 0 else 0.0
            p50, p95, p99 = percentiles(all_latencies)
            requests_per_conn = (
                round(len(all_latencies) / connections_opened, 3)
                if connections_opened > 0
                else None
            )

            workload_summary = {
                k: v for k, v in workload.items() if k not in ("body", "expected_body")
            }
            results.append(
                {
                    "condition": label,
                    "connection_mode": connection_mode,
                    "executable": str(executable),
                    "executable_sha256": sha256_file(executable),
                    "concurrency": concurrency,
                    "workload": workload_summary,
                    "total_attempts": len(all_latencies) + errors,
                    "successful": len(all_latencies),
                    "errors": errors,
                    "connections_opened": connections_opened,
                    "requests_per_connection": requests_per_conn,
                    "throughput_rps": round(throughput, 3),
                    "elapsed_ms": round(elapsed * 1000, 3),
                    "p50_ms": p50,
                    "p95_ms": p95,
                    "p99_ms": p99,
                    "error_samples": error_samples[:10],
                }
            )

    return results, server_proc, total_errors


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Phase 2 A/B/C performance benchmark harness.",
    )
    parser.add_argument("executable_a", type=Path, help="Phase 1 server executable")
    parser.add_argument("executable_b", type=Path, help="Phase 2 server executable")
    parser.add_argument("--port", type=int, default=None, help="server port (default: free port)")
    parser.add_argument(
        "--root",
        type=Path,
        default=None,
        help="parent directory for a temporary fixture root (default: system temp)",
    )
    parser.add_argument(
        "--concurrency",
        type=str,
        default=",".join(str(c) for c in DEFAULT_CONCURRENCY),
        help=f"comma-separated concurrency tiers (default: {','.join(str(c) for c in DEFAULT_CONCURRENCY)})",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=DEFAULT_REPEAT,
        help=f"measurement runs per worker (default: {DEFAULT_REPEAT})",
    )
    parser.add_argument(
        "--warmup",
        type=int,
        default=WARMUP_REQUESTS,
        help=f"warmup requests per worker (default: {WARMUP_REQUESTS})",
    )
    parser.add_argument(
        "--samples",
        type=int,
        default=SAMPLE_REQUESTS,
        help=f"sample requests per run (default: {SAMPLE_REQUESTS})",
    )
    parser.add_argument(
        "--max-requests-per-connection",
        type=int,
        default=DEFAULT_MAX_REQ_PER_CONN,
        help=f"rotate keep-alive connections after this many requests (default: {DEFAULT_MAX_REQ_PER_CONN})",
    )
    parser.add_argument(
        "--static-sizes",
        type=str,
        default="1024,65536",
        help="comma-separated static file sizes in bytes (default: 1024,65536)",
    )
    parser.add_argument(
        "--echo-sizes",
        type=str,
        default="0,1024,65536",
        help="comma-separated echo body sizes in bytes (default: 0,1024,65536)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="write JSON output to PATH instead of stdout",
    )
    parser.add_argument(
        "--log-file",
        type=Path,
        default=None,
        help="server log destination (default: temporary file; /dev/null allowed)",
    )
    parser.add_argument(
        "--server-arg",
        action="append",
        default=[],
        help="extra argument passed to every server (repeatable)",
    )
    args = parser.parse_args()

    if args.repeat < 1:
        parser.error("--repeat must be positive")
    if args.warmup < 0:
        parser.error("--warmup must be non-negative")
    if args.samples < 1:
        parser.error("--samples must be positive")
    if args.max_requests_per_connection < 1:
        parser.error("--max-requests-per-connection must be positive")

    executable_a = args.executable_a.resolve()
    executable_b = args.executable_b.resolve()
    for exe in (executable_a, executable_b):
        if not exe.is_file():
            print(f"error: executable not found: {exe}", file=sys.stderr)
            return 2

    try:
        concurrency_tiers = parse_int_list(args.concurrency)
        static_sizes = parse_int_list(args.static_sizes, allow_zero=True)
        echo_sizes = parse_int_list(args.echo_sizes, allow_zero=True)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.port is None:
        port = find_free_port()
    else:
        port = args.port
        if not (1 <= port <= 65535):
            print(f"error: port must be between 1 and 65535, got {port}", file=sys.stderr)
            return 2

    cleanup_root = tempfile.TemporaryDirectory(
        prefix="webserver-bench-root-", dir=args.root.resolve() if args.root else None
    )
    root = Path(cleanup_root.name)

    static_files = prepare_root(root, static_sizes)
    workloads: List[Dict[str, Any]] = []
    for path, size in static_files.items():
        expected = payload(size)
        workloads.append(
            {
                "name": f"static_{size}",
                "category": "static",
                "method": "GET",
                "path": path,
                "request_size": size,
                "body": None,
                "expected_body": expected,
            }
        )
    for size in echo_sizes:
        body = payload(size)
        workloads.append(
            {
                "name": f"echo_{size}",
                "category": "echo",
                "method": "POST",
                "path": "/echo",
                "request_size": size,
                "body": body,
                "expected_body": body,
            }
        )

    conditions = [
        {
            "label": "A",
            "executable": str(executable_a),
            "connection_mode": "close",
            "description": "Phase 1 blocking sequential server, Connection: close",
        },
        {
            "label": "B",
            "executable": str(executable_b),
            "connection_mode": "close",
            "description": "Phase 2 LT epoll server, Connection: close",
        },
        {
            "label": "C",
            "executable": str(executable_b),
            "connection_mode": "keep-alive",
            "description": "Phase 2 LT epoll server, keep-alive connection reuse",
        },
    ]

    all_results: List[Dict[str, Any]] = []
    total_errors = 0
    server_logs: Dict[str, Optional[str]] = {}
    server_proc: Optional[subprocess.Popen[bytes]] = None
    server_drain: Optional[threading.Thread] = None

    try:
        proc, drain, _q, log_a = start_server(
            executable_a, port, root, args.server_arg, args.log_file
        )
        server_proc, server_drain = proc, drain
        server_logs["A"] = str(log_a) if log_a else None
        results, server_proc, errors = run_condition(
            "A", executable_a, port, root, "close",
            concurrency_tiers, workloads, args.warmup, args.repeat, args.samples,
            args.max_requests_per_connection, server_proc,
        )
        all_results.extend(results)
        total_errors += errors
        stop_server(server_proc, server_drain)
        server_proc, server_drain = None, None

        proc, drain, _q, log_b = start_server(
            executable_b, port, root, args.server_arg, args.log_file
        )
        server_proc, server_drain = proc, drain
        server_logs["B"] = str(log_b) if log_b else None
        server_logs["C"] = server_logs["B"]

        results, server_proc, errors = run_condition(
            "B", executable_b, port, root, "close",
            concurrency_tiers, workloads, args.warmup, args.repeat, args.samples,
            args.max_requests_per_connection, server_proc,
        )
        all_results.extend(results)
        total_errors += errors

        results, server_proc, errors = run_condition(
            "C", executable_b, port, root, "keep-alive",
            concurrency_tiers, workloads, args.warmup, args.repeat, args.samples,
            args.max_requests_per_connection, server_proc,
        )
        all_results.extend(results)
        total_errors += errors

        stop_server(server_proc, server_drain)
        server_proc, server_drain = None, None

        missing_reuse = [
            {"concurrency": r["concurrency"], "workload": r["workload"]}
            for r in all_results
            if r["condition"] == "C"
            and args.max_requests_per_connection > 1
            and args.repeat * args.samples >= 2
            and r["successful"] <= r["connections_opened"]
        ]

        output = {
            "metadata": {
                "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                "hostname": platform.node(),
                "platform": platform.platform(),
                "python_version": platform.python_version(),
                "page_size": os.sysconf(os.sysconf_names.get("SC_PAGE_SIZE", 4096)),
                "host": HOST,
                "port": port,
                "root": str(root),
                "concurrency_tiers": concurrency_tiers,
                "warmup_requests": args.warmup,
                "sample_requests": args.samples,
                "repeat": args.repeat,
                "max_requests_per_connection": args.max_requests_per_connection,
                "static_sizes": static_sizes,
                "echo_sizes": echo_sizes,
                "executable_a": str(executable_a),
                "executable_a_sha256": sha256_file(executable_a),
                "executable_b": str(executable_b),
                "executable_b_sha256": sha256_file(executable_b),
                "server_args": ["--port", str(port), "--root", str(root)] + args.server_arg,
                "server_logs": server_logs,
            },
            "conditions": conditions,
            "summary": {
                "total_errors": total_errors,
                "total_successful_requests": sum(r["successful"] for r in all_results),
                "total_connections_opened": sum(r["connections_opened"] for r in all_results),
                "missing_reuse": missing_reuse,
            },
            "results": all_results,
        }

        json_text = json.dumps(output, indent=2, ensure_ascii=False)
        if args.output:
            args.output.write_text(json_text, encoding="utf-8")
        else:
            print(json_text)

        if total_errors:
            print(f"\nerror: {total_errors} request(s) failed", file=sys.stderr)
            return 1
        if missing_reuse:
            print("\nerror: condition C did not demonstrate connection reuse", file=sys.stderr)
            return 1
        return 0

    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        if server_proc is not None and server_drain is not None:
            stop_server(server_proc, server_drain)
        return 1
    finally:
        cleanup_root.cleanup()


if __name__ == "__main__":
    sys.exit(main())
