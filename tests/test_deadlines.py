#!/usr/bin/env python3
"""Real blocking-socket coverage with a dedicated short-deadline build."""

import http.client
import json
import os
import queue
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path
from typing import BinaryIO


def read_final(client: socket.socket) -> tuple[int, bytes]:
    """Read a final response and require framing to agree with its body."""
    with client.makefile("rb") as stream:
        status = int(stream.readline().split()[1])
        headers = http.client.parse_headers(stream)
        body = stream.read()
        assert len(body) == int(headers["Content-Length"])
    return status, body


def drain_lines(stream: BinaryIO, lines: queue.Queue[bytes]) -> None:
    for line in stream:
        lines.put(line)


def fd_count(pid: int) -> int:
    return len(os.listdir(f"/proc/{pid}/fd"))


def check_connection_lifetime(port: int, pid: int, lines: queue.Queue[bytes]) -> None:
    start_fds = fd_count(pid)
    with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
        with client.makefile("rb") as stream:
            wire = stream.read()
        assert wire.startswith(b"HTTP/1.1 408 Request Timeout\r\n"), wire
    header_log = json.loads(lines.get(timeout=5))
    assert (header_log["method"], header_log["path"], header_log["status"]) == (None, None, 408)
    assert header_log["complete"] and header_log["bytes_sent"] == len(wire)

    with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
        client.sendall(
            b"POST /echo?ignored=yes HTTP/1.1\r\nHost: localhost\r\n"
            b"Content-Length: 4\r\nExpect: 100-continue\r\n\r\n"
        )
        with client.makefile("rb") as stream:
            assert stream.readline() == b"HTTP/1.1 100 Continue\r\n"
            assert stream.readline() == b"\r\n"
            final = stream.read()
        assert final.startswith(b"HTTP/1.1 408 Request Timeout\r\n"), final
    body_log = json.loads(lines.get(timeout=5))
    assert (body_log["method"], body_log["path"], body_log["status"]) == ("POST", "/echo", 408)
    assert body_log["complete"] and body_log["bytes_sent"] == len(final)
    assert isinstance(body_log["duration_ms"], int)

    with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
        client.sendall(
            b"POST /echo?search=query-marker HTTP/1.1\r\nHost: localhost\r\n"
            b"Connection: close\r\n"
            b"Authorization: Bearer auth-marker\r\nCookie: example=cookie-marker\r\n"
            b"Content-Length: 11\r\n\r\nbody-marker"
        )
        assert read_final(client) == (200, b"body-marker")
    raw = lines.get(timeout=5)
    assert json.loads(raw)["path"] == "/echo"
    assert not any(marker in raw for marker in (
        b"query-marker", b"auth-marker", b"cookie-marker", b"body-marker"
    ))

    for _ in range(30):
        # Every log is emitted after the socket and file descriptors are closed.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
            client.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
            assert read_final(client) == (200, b"next request")
        record = json.loads(lines.get(timeout=5))
        assert record["complete"] and record["path"] == "/" and record["status"] == 200
        assert fd_count(pid) == start_fds


def main() -> None:
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        (root / "index.html").write_bytes(b"next request")
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        with subprocess.Popen(
            [str(exe), "--port", str(port), "--root", str(root)],
            stdout=subprocess.PIPE,
        ) as proc:
            assert proc.stdout is not None
            lines: queue.Queue[bytes] = queue.Queue()
            reader = threading.Thread(target=drain_lines, args=(proc.stdout, lines), daemon=True)
            reader.start()
            try:
                assert lines.get(timeout=5) == f"READY {port}\n".encode("ascii")
                check_connection_lifetime(port, proc.pid, lines)
            finally:
                if proc.poll() is None:
                    proc.terminate()
                proc.wait(timeout=5)
                reader.join(timeout=5)
    print("DEADLINE_INTEGRATION_TESTS_PASS")


if __name__ == "__main__":
    main()
