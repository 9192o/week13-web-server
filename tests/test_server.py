#!/usr/bin/env python3
import http.client
import queue
import socket
import subprocess
import sys
import threading
import time
from typing import Final

HOST: Final = "127.0.0.1"
PORT: Final = 8080
READY_TIMEOUT: Final = 5.0
REQUEST_TIMEOUT: Final = 5.0
TERM_TIMEOUT: Final = 5.0


def stop_server(proc: subprocess.Popen[bytes], drain_thread: threading.Thread) -> None:
    proc.terminate()
    try:
        proc.wait(timeout=TERM_TIMEOUT)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=TERM_TIMEOUT)
    drain_thread.join(timeout=TERM_TIMEOUT)
    assert not drain_thread.is_alive(), "stdout drain did not finish"


def start_server(exe: str) -> tuple[subprocess.Popen[bytes], threading.Thread]:
    proc = subprocess.Popen(
        [exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL
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
        if b"Waiting for client" in line:
            return proc, t

    stop_server(proc, t)
    raise AssertionError("server did not become ready")


def exchange(data: bytes, expected: int, *, shutdown_write: bool = False) -> None:
    with socket.create_connection((HOST, PORT), timeout=REQUEST_TIMEOUT) as sock:
        if data:
            sock.sendall(data)
        if shutdown_write:
            sock.shutdown(socket.SHUT_WR)
        with http.client.HTTPResponse(sock) as response:
            response.begin()
            assert response.version == 11
            assert response.status == expected, (response.status, expected)
            assert response.getheader("Content-Length") == "0"
            assert response.getheader("Connection") == "close"
            assert response.read() == b""


def read_head(sock: socket.socket) -> bytes:
    head = bytearray()
    while not head.endswith(b"\r\n\r\n"):
        byte = sock.recv(1)
        assert byte, "EOF before response headers"
        head.extend(byte)
        assert len(head) <= 32768
    return bytes(head)


def expect_continue(chunked: bool) -> None:
    body = b"a\0b\xffz"
    framing = b"Transfer-Encoding: chunked" if chunked else b"Content-Length: 5"
    wire = b"5\r\n" + body + b"\r\n0\r\n\r\n" if chunked else body
    with socket.create_connection((HOST, PORT), timeout=REQUEST_TIMEOUT) as sock:
        sock.sendall(
            b"POST /x HTTP/1.1\r\nHost: x\r\n"
            + framing
            + b"\r\nExpect: 100-continue\r\n\r\n"
        )
        assert read_head(sock) == b"HTTP/1.1 100 Continue\r\n\r\n"
        sock.sendall(wire)
        with http.client.HTTPResponse(sock) as response:
            response.begin()
            assert response.status == 200
            assert response.getheader("Content-Length") == "0"
            assert response.getheader("Connection") == "close"
            assert response.read() == b""


def main() -> None:
    if len(sys.argv) != 2:
        print("usage: test_server.py <path-to-webserver-executable>", file=sys.stderr)
        sys.exit(2)

    exe = sys.argv[1]
    proc, drain_thread = start_server(exe)
    try:
        exchange(b"GET /arbitrary/path HTTP/1.1\r\n\r\n", 200)
        exchange(b"CUSTOM /x HTTP/1.1\r\n\r\n", 200)
        exchange(b"GET / HTTP/1.1 extra\r\n\r\n", 400)

        target_len = 8192 - len(b"GET /") - len(b" HTTP/1.1\r\n")
        exact = b"GET /" + b"A" * target_len + b" HTTP/1.1\r\n"
        assert len(exact) == 8192
        exchange(exact + b"\r\n", 200)

        too_long = b"GET /" + b"A" * (target_len + 1) + b" HTTP/1.1\r\n"
        assert len(too_long) == 8193
        exchange(too_long, 414)
        exchange(b"GET / HTTP/1.1\r", 400, shutdown_write=True)
        exchange(b"", 400, shutdown_write=True)
        exchange(b"GET / HTTP/1.1\r\nHost: x", 400, shutdown_write=True)

        body = bytes(range(256)) * 16
        prefix = b"POST /x HTTP/1.1\r\nHost: x\r\n"
        exchange(
            prefix + b"cOnTeNt-LeNgTh: 4096\r\n\r\n" + body,
            200,
        )
        exchange(
            prefix + b"Transfer-Encoding: chunked\r\n\r\n"
            + b"1000;x=\"a,b\"\r\n" + body
            + b"\r\n0\r\nX-Note: ignored\r\n\r\n",
            200,
        )
        maximum = bytes(range(256)) * 4096
        exchange(prefix + b"Content-Length: 1048576\r\n\r\n" + maximum, 200)
        exchange(
            prefix + b"Transfer-Encoding: chunked\r\n\r\n100000\r\n"
            + maximum + b"\r\n0\r\n\r\n",
            200,
        )
        for fields, status in [
            (b"Content-Length: 1048577", 413),
            (b"Content-Length: 1\r\nContent-Length: 1", 400),
            (b"Content-Length: 1, 1", 400),
            (b"Content-Length: +1", 400),
            (b"Content-Length: 184467440737095516160", 400),
            (b"Content-Length: 1\r\nTransfer-Encoding: chunked", 400),
            (b"Transfer-Encoding: gzip, chunked", 501),
            (b"Transfer-Encoding: chunked, gzip", 400),
            (b"Transfer-Encoding: chunked, chunked", 400),
            (b"Expect: unknown", 417),
            (b"Expect: bad value", 400),
            (b"Bad(Name: x", 400),
        ]:
            exchange(prefix + fields + b"\r\n\r\n", status)
        exchange(prefix + b"X: a\r\n" * 101 + b"\r\n", 431)
        exchange(prefix + b"Content-Length: 5\r\n\r\nab", 400, shutdown_write=True)
        exchange(
            prefix + b"Transfer-Encoding: chunked\r\n\r\n5\r\nab",
            400, shutdown_write=True,
        )
        exchange(
            prefix + b"Transfer-Encoding: chunked\r\n\r\n0\r\nX: a\r",
            400, shutdown_write=True,
        )
        exchange(prefix + b"Transfer-Encoding: chunked\r\n\r\n100001\r\n", 413)
        exchange(
            prefix + b"Content-Length: 1048577\r\nExpect: 100-continue\r\n\r\n",
            413,
        )
        expect_continue(False)
        expect_continue(True)
        exchange(b"GET /ok HTTP/1.1\r\n\r\n", 200)

        print("INTEGRATION_TESTS_PASS")
    finally:
        stop_server(proc, drain_thread)


if __name__ == "__main__":
    main()
