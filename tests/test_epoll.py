#!/usr/bin/env python3
"""Real socket regression for Phase 2 epoll-based concurrency.

Accepts the server binary path as the only positional argument:

    python3 tests/test_epoll.py <path-to-webserver-executable>

On success prints ``EPOLL_TESTS_PASS`` and exits 0.
On failure prints the failing assertion and exits 1.

This module uses only the Python standard library and ordinary local socket
requests.  Most requests carry ``Connection: close``; the truncated-response
case leaves reuse enabled to verify that the server stops the pipeline.
The suite covers the Phase 2 contract (P2-01, P2-02, P2-09).
"""

import http.client
import os
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Optional

HOST: str = "127.0.0.1"
READY_TIMEOUT: float = 5.0
REQUEST_TIMEOUT: float = 8.0
TERM_TIMEOUT: float = 5.0
CONNECT_TIMEOUT: float = 2.0
PARTIAL_SEND: bytes = b"GET / HTTP/1.1\r\nHo"
INDEX: bytes = b"epoll concurrency test index\n"
LARGE_SIZE: int = 1 * 1024 * 1024


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


def start_server(
    exe: str,
    root: str,
    port: Optional[int] = None,
    max_connections: Optional[int] = None,
) -> tuple[subprocess.Popen[bytes], threading.Thread, queue.Queue[bytes | None], int]:
    """Start the server, wait for ``READY <port>``, and return a drain thread."""
    if port is None:
        port = find_free_port()
    args = [exe, "--port", str(port), "--root", root]
    if max_connections is not None:
        args.extend(["--max-connections", str(max_connections)])

    proc = subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
    )
    q: queue.Queue[bytes | None] = queue.Queue()
    stdout = proc.stdout
    assert stdout is not None

    def drain() -> None:
        try:
            with stdout:
                for line in iter(stdout.readline, b""):
                    q.put(line)
        finally:
            q.put(None)

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
        parts = line.strip().split()
        if len(parts) == 2 and parts[0] == b"READY":
            try:
                ready_port = int(parts[1].decode())
            except ValueError:
                break
            if ready_port == port:
                return proc, t, q, port

    stop_server(proc, t)
    raise AssertionError("server did not become ready")


def stop_server(proc: subprocess.Popen[bytes], drain_thread: threading.Thread) -> None:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=TERM_TIMEOUT)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=TERM_TIMEOUT)
    drain_thread.join(timeout=TERM_TIMEOUT)


def fd_count(pid: int) -> int:
    return len(os.listdir(f"/proc/{pid}/fd"))


def wait_for_fd_count(
    pid: int, expected: int, timeout: float = 2.0
) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if fd_count(pid) == expected:
            return
        time.sleep(0.05)
    actual = fd_count(pid)
    assert actual == expected, f"FD count {actual} != expected {expected}"


def read_response(
    sock: socket.socket, method: str = "GET"
) -> tuple[int, http.client.HTTPMessage, bytes]:
    with http.client.HTTPResponse(sock, method=method) as response:
        response.begin()
        body = response.read()
        return response.status, response.headers, body


def assert_response(
    sock: socket.socket, expected_status: int, expected_body: bytes, method: str = "GET"
) -> None:
    status, headers, body = read_response(sock, method=method)
    assert status == expected_status, (status, expected_status)
    assert body == expected_body, (len(body), len(expected_body))
    assert headers.get("Connection") == "close", headers.get("Connection")


def send_rst(sock: socket.socket) -> None:
    """Close a socket by sending RST instead of a clean FIN."""
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    sock.close()


def test_slow_a_fast_b(exe: str, root: str) -> None:
    """P2-01: while A holds a partial POST /echo body, B's GET / completes."""
    proc, drain_thread, _, port = start_server(exe, root)
    try:
        body = b"abcdefghij"
        a = socket.create_connection((HOST, port), timeout=REQUEST_TIMEOUT)
        try:
            a.sendall(
                b"POST /echo HTTP/1.1\r\nHost: x\r\n"
                b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                b"Connection: close\r\n\r\n"
                b"abcd"
            )

            b = socket.create_connection((HOST, port), timeout=REQUEST_TIMEOUT)
            try:
                b.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
                b.settimeout(3.0)
                assert_response(b, 200, INDEX)
            finally:
                b.close()

            a.sendall(b"efghij")
            assert_response(a, 200, body)
        finally:
            a.close()
    finally:
        stop_server(proc, drain_thread)
    print("PASS SLOW_A_FAST_B")


def check_connection_limit(
    exe: str, root: str, limit: int, use_option: bool
) -> None:
    """P2-02: hold ``limit`` partial connections, then an extra one queues."""
    if use_option:
        proc, drain_thread, _, port = start_server(exe, root, max_connections=limit)
    else:
        proc, drain_thread, _, port = start_server(exe, root)
    try:
        baseline = fd_count(proc.pid)
        held: list[socket.socket] = []
        try:
            for _ in range(limit):
                s = socket.create_connection((HOST, port), timeout=CONNECT_TIMEOUT)
                s.sendall(PARTIAL_SEND)
                held.append(s)
            wait_for_fd_count(proc.pid, baseline + limit)

            extra = socket.create_connection((HOST, port), timeout=CONNECT_TIMEOUT)
            try:
                extra.sendall(
                    b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
                )
                extra.settimeout(0.5)
                try:
                    data = extra.recv(1)
                except socket.timeout:
                    data = b""
                assert not data, f"extra connection got data before slot freed: {data!r}"

                held[0].close()
                held = held[1:]

                extra.settimeout(REQUEST_TIMEOUT)
                assert_response(extra, 200, INDEX)
            finally:
                extra.close()
        finally:
            for s in held:
                s.close()
    finally:
        stop_server(proc, drain_thread)
    print(f"PASS CONNECTION_LIMIT_{limit}")


def test_max_connections_two(exe: str, root: str) -> None:
    check_connection_limit(exe, root, limit=2, use_option=True)


def test_default_limit(exe: str, root: str) -> None:
    """P2-02: the default connection limit is 256."""
    check_connection_limit(exe, root, limit=256, use_option=False)


def test_shutdown_honors_connection_limit(exe: str, root: str) -> None:
    """P2-02 + P1-27: after shutdown starts, cap=2 still refuses new accepts."""
    proc, drain_thread, _, port = start_server(exe, root, max_connections=2)
    try:
        held = socket.create_connection((HOST, port), timeout=CONNECT_TIMEOUT)
        try:
            held.sendall(PARTIAL_SEND)
            time.sleep(0.15)

            proc.terminate()
            time.sleep(0.15)

            late = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            late.settimeout(1.0)
            try:
                late.connect((HOST, port))
            except (socket.timeout, ConnectionRefusedError):
                late.close()
            else:
                try:
                    late.settimeout(1.0)
                    try:
                        data = late.recv(1024)
                    except socket.timeout:
                        data = b""
                    assert data == b"", f"late connection got response during shutdown: {data!r}"
                finally:
                    late.close()
        finally:
            held.close()
    finally:
        stop_server(proc, drain_thread)
    print("PASS SHUTDOWN_HONORS_CONNECTION_LIMIT")


def test_interruptions_and_fd_baseline(exe: str, root: str) -> None:
    """P2-09: resets during receive/send do not affect other clients or FDs."""
    root_path = Path(root)
    large = bytes(range(256)) * (LARGE_SIZE // 256)
    (root_path / "large.bin").write_bytes(large)

    proc, drain_thread, _, port = start_server(exe, root)
    baseline = fd_count(proc.pid)
    try:
        errors: list[BaseException] = []
        results: list[tuple[int, bytes]] = []
        lock = threading.Lock()

        def normal_client() -> None:
            try:
                with socket.create_connection(
                    (HOST, port), timeout=REQUEST_TIMEOUT
                ) as s:
                    s.sendall(
                        b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
                    )
                    status, _, body = read_response(s)
                    with lock:
                        results.append((status, body))
            except BaseException as exc:
                with lock:
                    errors.append(exc)

        def reset_during_receive() -> None:
            try:
                s = socket.create_connection((HOST, port), timeout=CONNECT_TIMEOUT)
                try:
                    s.sendall(
                        b"POST /echo HTTP/1.1\r\nHost: x\r\n"
                        b"Content-Length: 100\r\n"
                        b"Connection: close\r\n\r\n"
                        b"partial-body"
                    )
                    time.sleep(0.05)
                finally:
                    send_rst(s)
            except BaseException as exc:
                with lock:
                    errors.append(exc)

        def reset_during_send() -> None:
            try:
                s = socket.create_connection((HOST, port), timeout=CONNECT_TIMEOUT)
                try:
                    s.sendall(
                        b"GET /large.bin HTTP/1.1\r\n"
                        b"Host: x\r\n"
                        b"Connection: close\r\n\r\n"
                    )
                    head = bytearray()
                    while not head.endswith(b"\r\n\r\n"):
                        byte = s.recv(1)
                        assert byte, "EOF before end of headers"
                        head.extend(byte)
                    s.recv(4096)
                finally:
                    send_rst(s)
            except BaseException as exc:
                with lock:
                    errors.append(exc)

        for _ in range(5):
            threads = [
                threading.Thread(target=normal_client),
                threading.Thread(target=reset_during_receive),
                threading.Thread(target=reset_during_send),
            ]
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=10.0)
                assert not t.is_alive(), "client thread hung"
            assert not errors, f"client errors: {errors}"
            assert results and results[-1] == (200, INDEX), results[-1:]
            results.clear()

        wait_for_fd_count(proc.pid, baseline)
    finally:
        stop_server(proc, drain_thread)
    print("PASS INTERRUPTIONS_AND_FD_BASELINE")


def test_truncated_response_closes_pipeline(exe: str, root: str) -> None:
    """A short file response must end the connection, not start the next response."""
    path = Path(root) / "truncated.png"
    size = 32 * 1024 * 1024
    with path.open("wb") as file:
        file.truncate(size)
        proc, drain_thread, _, port = start_server(exe, root)
        try:
            baseline = fd_count(proc.pid)
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024)
                sock.settimeout(REQUEST_TIMEOUT)
                sock.connect((HOST, port))
                sock.sendall(
                    b"GET /truncated.png HTTP/1.1\r\nHost: x\r\n\r\n"
                    b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
                )
                with http.client.HTTPResponse(sock) as response:
                    response.begin()
                    assert response.status == 200
                    assert response.getheader("Content-Length") == str(size)
                    assert response.read(1) == b"\0"
                    file.truncate(0)
                    try:
                        response.read()
                    except http.client.IncompleteRead as exc:
                        assert not exc.partial.strip(b"\0"), "unexpected response after file bytes"
                    else:
                        raise AssertionError("expected a short body followed by connection close")
                    assert sock.recv(1) == b"", "connection did not end after the short body"
            wait_for_fd_count(proc.pid, baseline)
        finally:
            stop_server(proc, drain_thread)
        assert proc.returncode == 0, proc.returncode
    print("PASS TRUNCATED_RESPONSE_CLOSES_PIPELINE")


def main() -> None:
    if len(sys.argv) != 2:
        print("usage: test_epoll.py <path-to-webserver-executable>", file=sys.stderr)
        sys.exit(2)

    exe = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="webserver-epoll-") as root:
        (Path(root) / "index.html").write_bytes(INDEX)
        test_slow_a_fast_b(exe, root)
        test_max_connections_two(exe, root)
        test_default_limit(exe, root)
        test_shutdown_honors_connection_limit(exe, root)
        test_interruptions_and_fd_baseline(exe, root)
        test_truncated_response_closes_pipeline(exe, root)

    print("EPOLL_TESTS_PASS")


if __name__ == "__main__":
    main()
