#!/usr/bin/env python3
from __future__ import annotations

import argparse
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
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Dict, Final, List, Optional, Tuple

HOST: Final = "127.0.0.1"
READY_TIMEOUT: Final = 10.0
REQUEST_TIMEOUT: Final = 60.0
TERM_TIMEOUT: Final = 5.0

WARMUP_REQUESTS: Final = 1
SAMPLE_REQUESTS: Final = 30
DEFAULT_REPEAT: Final = 3

CHUNK_SIZE: Final = 8192

STATIC_PREFIX: Final = "webserver_bench_"


def payload(size: int) -> bytes:
    pattern = bytes(range(256))
    n, r = divmod(size, 256)
    return pattern * n + pattern[:r]


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((HOST, 0))
        return int(sock.getsockname()[1])


def chunked_encode(data: bytes, chunk_size: int = CHUNK_SIZE) -> bytes:
    out = bytearray()
    for i in range(0, len(data), chunk_size):
        chunk = data[i : i + chunk_size]
        out.extend(f"{len(chunk):X}\r\n".encode())
        out.extend(chunk)
        out.extend(b"\r\n")
    out.extend(b"0\r\n\r\n")
    return bytes(out)


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


def get_proc_stats(pid: int) -> Optional[Dict[str, int]]:
    try:
        page_size = os.sysconf(os.sysconf_names.get("SC_PAGE_SIZE", 4096))
    except (AttributeError, OSError, ValueError):
        page_size = 4096

    try:
        with open(f"/proc/{pid}/statm", "rb") as f:
            parts = f.read().split()
            rss_pages = int(parts[1]) if len(parts) > 1 else 0
    except (OSError, ValueError):
        return None

    try:
        fd_count = sum(1 for _ in os.scandir(f"/proc/{pid}/fd"))
    except (OSError, ValueError):
        fd_count = 0

    return {
        "rss_bytes": rss_pages * page_size,
        "fd_count": fd_count,
    }


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


def send_and_receive(request: bytes) -> Tuple[int, http.client.HTTPMessage, bytes]:
    with socket.create_connection((HOST, _current_port[0]), timeout=REQUEST_TIMEOUT) as sock:
        sock.sendall(request)
        response = http.client.HTTPResponse(sock)
        response.begin()
        body = response.read()
        return response.status, response.msg, body


_current_port: List[int] = []


def request_once(
    method: str,
    path: str,
    headers: Optional[Dict[str, str]] = None,
    body: Optional[bytes] = None,
    expected_body: Optional[bytes] = None,
) -> Tuple[Optional[float], Optional[str]]:
    req = build_request(method, path, headers, body)
    start = time.perf_counter()
    try:
        status, _msg, resp_body = send_and_receive(req)
    except Exception as exc:
        elapsed = time.perf_counter() - start
        return round(elapsed * 1000, 3), f"{type(exc).__name__}: {exc}"
    elapsed = time.perf_counter() - start
    if status != 200:
        return round(elapsed * 1000, 3), f"status {status}"
    if expected_body is not None and resp_body != expected_body:
        return (
            round(elapsed * 1000, 3),
            f"body mismatch (got {len(resp_body)} bytes, expected {len(expected_body)} bytes)",
        )
    return round(elapsed * 1000, 3), None


def run_measurement(
    make_request: Callable[[], Tuple[Optional[float], Optional[str]]],
    repeat: int,
    warmup: int,
    samples: int,
) -> Tuple[List[Dict[str, Any]], int]:
    results: List[Dict[str, Any]] = []
    total_errors = 0
    for run in range(1, repeat + 1):
        for _ in range(warmup):
            _latency, err = make_request()
            if err:
                raise RuntimeError(f"warmup failed: {err}")

        latencies: List[float] = []
        errors = 0
        t0 = time.perf_counter()
        for _ in range(samples):
            latency, err = make_request()
            if err or latency is None:
                errors += 1
                total_errors += 1
            else:
                latencies.append(latency)
        t1 = time.perf_counter()
        elapsed = t1 - t0

        p50, p95, p99 = percentiles(latencies)
        throughput = len(latencies) / elapsed if elapsed > 0 else 0.0

        results.append(
            {
                "run": run,
                "successful": len(latencies),
                "errors": errors,
                "throughput_rps": round(throughput, 3),
                "elapsed_ms": round(elapsed * 1000, 3),
                "p50_ms": p50,
                "p95_ms": p95,
                "p99_ms": p99,
            }
        )
    return results, total_errors


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
            _current_port[0] = ready_port
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
    proc.terminate()
    try:
        proc.wait(timeout=TERM_TIMEOUT)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=TERM_TIMEOUT)
    drain_thread.join(timeout=TERM_TIMEOUT)


def update_peak(peak: Dict[str, int], current: Optional[Dict[str, int]]) -> None:
    if current is None:
        return
    peak["rss_bytes"] = max(peak["rss_bytes"], current.get("rss_bytes", 0))
    peak["fd_count"] = max(peak["fd_count"], current.get("fd_count", 0))


def prepare_root(root: Path) -> Dict[str, Tuple[str, int]]:
    files = [
        (f"{STATIC_PREFIX}static_1k.png", 1024),
        (f"{STATIC_PREFIX}static_64k.png", 64 * 1024),
        (f"{STATIC_PREFIX}static_2m.png", 2 * 1024 * 1024),
    ]
    mapping: Dict[str, Tuple[str, int]] = {}
    for name, size in files:
        path = root / name
        with path.open("xb") as file:
            file.write(payload(size))
        mapping[name] = (f"/{name}", size)
    return mapping


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Phase 1 performance baseline harness for the web server.",
    )
    parser.add_argument("executable", type=Path, help="path to the webserver executable")
    parser.add_argument("--port", type=int, default=None, help="server port (default: free port)")
    parser.add_argument(
        "--root",
        type=Path,
        default=None,
        help="parent directory for a temporary fixture root (default: system temp)",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=DEFAULT_REPEAT,
        help=f"measurement runs per workload (default: {DEFAULT_REPEAT})",
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
        help="extra argument passed to the server (repeatable)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="write JSON output to PATH instead of stdout",
    )
    parser.add_argument(
        "--echo-only",
        action="store_true",
        help="run only echo workloads (useful before general static file serving is ready)",
    )
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")

    executable = args.executable.resolve()
    if not executable.is_file():
        print(f"error: executable not found: {executable}", file=sys.stderr)
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

    static_files = prepare_root(root) if not args.echo_only else {}

    _current_port.clear()
    _current_port.append(port)

    proc: Optional[subprocess.Popen[bytes]] = None
    drain_thread: Optional[threading.Thread] = None
    try:
        proc, drain_thread, _q, log_file = start_server(
            executable, port, root, args.server_arg, args.log_file
        )
        pid = proc.pid

        baseline = get_proc_stats(pid)
        peak = dict(baseline or {"rss_bytes": 0, "fd_count": 0})

        stop_sampling = threading.Event()

        def sampler() -> None:
            while not stop_sampling.wait(0.05):
                update_peak(peak, get_proc_stats(pid))

        sampler_thread = threading.Thread(target=sampler, daemon=True)
        sampler_thread.start()

        workloads: List[Dict[str, Any]] = []
        total_errors = 0
        total_successful = 0

        if not args.echo_only:
            for name, (path, size) in static_files.items():
                expected = payload(size)

                def make_static(p: str = path, s: int = size, exp: bytes = expected) -> Tuple[Optional[float], Optional[str]]:
                    return request_once("GET", p, expected_body=exp)

                results, errors = run_measurement(make_static, args.repeat, WARMUP_REQUESTS, SAMPLE_REQUESTS)
                total_errors += errors
                for r in results:
                    total_successful += r["successful"]
                workloads.append(
                    {
                        "name": f"{name[:-4]}_get",
                        "category": "static",
                        "encoding": None,
                        "path": path,
                        "request_size": size,
                        "repeat": args.repeat,
                        "warmup_requests": WARMUP_REQUESTS,
                        "sample_requests": SAMPLE_REQUESTS,
                        "results": results,
                    }
                )
                update_peak(peak, get_proc_stats(pid))

        for size in (0, 1024, 64 * 1024, 1024 * 1024):
            body = payload(size)

            def make_echo_cl(s: int = size, b: bytes = body) -> Tuple[Optional[float], Optional[str]]:
                return request_once(
                    "POST",
                    "/echo",
                    headers={"Content-Length": str(s)},
                    body=b,
                    expected_body=b,
                )

            results, errors = run_measurement(make_echo_cl, args.repeat, WARMUP_REQUESTS, SAMPLE_REQUESTS)
            total_errors += errors
            for r in results:
                total_successful += r["successful"]
            workloads.append(
                {
                    "name": f"echo_cl_{size}",
                    "category": "echo",
                    "encoding": "cl",
                    "path": "/echo",
                    "request_size": size,
                    "repeat": args.repeat,
                    "warmup_requests": WARMUP_REQUESTS,
                    "sample_requests": SAMPLE_REQUESTS,
                    "results": results,
                }
            )
            update_peak(peak, get_proc_stats(pid))

        for size in (0, 1024, 64 * 1024, 1024 * 1024):
            body = payload(size)
            chunked = chunked_encode(body)

            def make_echo_chunked(
                ch: bytes = chunked, b: bytes = body
            ) -> Tuple[Optional[float], Optional[str]]:
                return request_once(
                    "POST",
                    "/echo",
                    headers={"Transfer-Encoding": "chunked"},
                    body=ch,
                    expected_body=b,
                )

            results, errors = run_measurement(make_echo_chunked, args.repeat, WARMUP_REQUESTS, SAMPLE_REQUESTS)
            total_errors += errors
            for r in results:
                total_successful += r["successful"]
            workloads.append(
                {
                    "name": f"echo_chunked_{size}",
                    "category": "echo",
                    "encoding": "chunked",
                    "path": "/echo",
                    "request_size": size,
                    "repeat": args.repeat,
                    "warmup_requests": WARMUP_REQUESTS,
                    "sample_requests": SAMPLE_REQUESTS,
                    "results": results,
                }
            )
            update_peak(peak, get_proc_stats(pid))

        final = get_proc_stats(pid)
        stop_sampling.set()
        sampler_thread.join(timeout=1.0)
        stop_server(proc, drain_thread)
        proc = None
        drain_thread = None

        output = {
            "metadata": {
                "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                "hostname": platform.node(),
                "platform": platform.platform(),
                "python_version": platform.python_version(),
                "page_size": os.sysconf(os.sysconf_names.get("SC_PAGE_SIZE", 4096)),
                "server_executable": str(executable),
                "server_args": [str(executable), "--port", str(port), "--root", str(root)]
                + args.server_arg,
                "server_log_file": str(log_file) if log_file else None,
                "host": HOST,
                "port": port,
                "root": str(root),
                "repeat": args.repeat,
                "warmup_requests": WARMUP_REQUESTS,
                "sample_requests": SAMPLE_REQUESTS,
                "echo_only": args.echo_only,
            },
            "environment": {
                "rss_bytes_before": baseline.get("rss_bytes") if baseline else None,
                "fd_count_before": baseline.get("fd_count") if baseline else None,
                "rss_bytes_peak": peak.get("rss_bytes"),
                "fd_count_peak": peak.get("fd_count"),
                "rss_bytes_after": final.get("rss_bytes") if final else None,
                "fd_count_after": final.get("fd_count") if final else None,
            },
            "summary": {
                "total_errors": total_errors,
                "total_successful_requests": total_successful,
            },
            "workloads": workloads,
        }

        json_text = json.dumps(output, indent=2, ensure_ascii=False)
        if args.output:
            args.output.write_text(json_text, encoding="utf-8")
        else:
            print(json_text)

        if total_errors:
            print(f"\nerror: {total_errors} request(s) failed", file=sys.stderr)
            return 1
        return 0

    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        if proc is not None and drain_thread is not None:
            stop_server(proc, drain_thread)
        return 1
    finally:
        cleanup_root.cleanup()


if __name__ == "__main__":
    sys.exit(main())
