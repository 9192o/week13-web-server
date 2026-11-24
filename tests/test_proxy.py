#!/usr/bin/env python3
"""Deterministic loopback integration tests for the fixed-config reverse proxy.

    python3 tests/test_proxy.py <path-to-reverse-proxy-executable>

Covers routing/boundary fallback, rewritten Host, binary POST with query,
backend status/type/bytes relay, nonblocking upstream, streaming, upstream
framing variants, 502/504 failures, disconnect FD recovery and graceful shutdown.
Only the Python standard library is used.
"""

import http.client
import os
import queue
import socket
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional

HOST: str = "127.0.0.1"
READY_TIMEOUT: float = 5.0
REQUEST_TIMEOUT: float = 8.0
TERM_TIMEOUT: float = 5.0
CONNECT_TIMEOUT: float = 2.0


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


def start_server(
    exe: str,
    routes: list[tuple[str, str]],
) -> tuple[subprocess.Popen[bytes], threading.Thread, queue.Queue[bytes | None], int]:
    port = find_free_port()
    args = [exe, "--port", str(port)]
    for prefix, backend in routes:
        args.extend(["--route", f"{prefix}={backend}"])
    return start_process(args, port)


def start_process(
    args: list[str], port: int
) -> tuple[subprocess.Popen[bytes], threading.Thread, queue.Queue[bytes | None], int]:
    """Start an HTTP process and wait for its listener-ready notification."""
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
    startup_output = []
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
        startup_output.append(line)
        parts = line.strip().split()
        if len(parts) == 2 and parts[0] == b"READY":
            try:
                ready_port = int(parts[1].decode())
            except ValueError:
                break
            if ready_port == port:
                return proc, t, q, port

    stop_server(proc, t, check=False)
    raise AssertionError(f"server did not become ready: {b''.join(startup_output)!r}")


def stop_server(
    proc: subprocess.Popen[bytes], drain_thread: threading.Thread, *, check: bool = True
) -> None:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=TERM_TIMEOUT)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=TERM_TIMEOUT)
    drain_thread.join(timeout=TERM_TIMEOUT)
    if check:
        assert proc.returncode == 0, f"server exit status: {proc.returncode}"


def fd_count(pid: int) -> int:
    return len(os.listdir(f"/proc/{pid}/fd"))


def wait_for_fd_count(pid: int, expected: int, timeout: float = 2.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if fd_count(pid) == expected:
            return
        time.sleep(0.05)
    actual = fd_count(pid)
    assert actual == expected, f"FD count {actual} != expected {expected}"


class Response:
    def __init__(
        self,
        status: int,
        headers: http.client.HTTPMessage,
        body: bytes,
    ) -> None:
        self.status = status
        self.headers = headers
        self.body = body


def proxy_request(
    port: int,
    request: bytes,
    timeout: float = REQUEST_TIMEOUT,
) -> Response:
    method = request.split(b" ", 1)[0].decode()
    with socket.create_connection((HOST, port), timeout=timeout) as sock:
        sock.sendall(request)
        with http.client.HTTPResponse(sock, method=method) as response:
            response.begin()
            body = response.read()
            return Response(response.status, response.msg, body)


def send_rst(sock: socket.socket) -> None:
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    sock.close()


def _read_chunked_body(handler: BaseHTTPRequestHandler) -> bytes:
    body = bytearray()
    while True:
        line = handler.rfile.readline()
        if not line:
            break
        line = line.strip()
        if not line:
            continue
        semi = line.find(b";")
        if semi >= 0:
            line = line[:semi]
        try:
            chunk_size = int(line, 16)
        except ValueError:
            break
        if chunk_size == 0:
            handler.rfile.readline()
            break
        remaining = chunk_size
        while remaining > 0:
            chunk = handler.rfile.read(remaining)
            if not chunk:
                break
            body.extend(chunk)
            remaining -= len(chunk)
        handler.rfile.readline()
    return bytes(body)


def _read_body(handler: BaseHTTPRequestHandler) -> bytes:
    cl = handler.headers.get("Content-Length")
    if cl is not None:
        try:
            length = int(cl)
        except ValueError:
            return b""
        if length < 0:
            return b""
        return handler.rfile.read(length)
    te = handler.headers.get("Transfer-Encoding")
    if te and "chunked" in te.lower():
        return _read_chunked_body(handler)
    return b""


class BackendHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: object) -> None:
        pass

    def _handler_for(self, method: str):
        server = self.server
        with server.lock:
            handlers = dict(server.handlers)
        key = (method, self.path)
        if key in handlers:
            return handlers[key]
        key = (method, self.path.split("?", 1)[0])
        return handlers.get(key)

    def do_GET(self) -> None:
        self._handle("GET")

    def do_HEAD(self) -> None:
        self._handle("HEAD")

    def do_POST(self) -> None:
        self._handle("POST")

    def _handle(self, method: str) -> None:
        handler = self._handler_for(method)
        if handler is None:
            self.send_error(404)
            return
        handler(self)


class BackendServer:
    def __init__(self) -> None:
        self.server = ThreadingHTTPServer((HOST, 0), BackendHandler)
        self.server.allow_reuse_address = True
        self.port = self.server.server_address[1]
        self.server.handlers: dict[tuple[str, str], callable] = {}
        self.server.lock = threading.Lock()
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2.0)


def backend_send(
    handler: BaseHTTPRequestHandler,
    status: int,
    body: bytes,
    content_type: str = "application/octet-stream",
    extra_headers: Optional[list[tuple[str, str]]] = None,
) -> None:
    handler.send_response(status)
    handler.send_header("Content-Type", content_type)
    handler.send_header("Content-Length", str(len(body)))
    if extra_headers:
        for name, value in extra_headers:
            handler.send_header(name, value)
    handler.end_headers()
    handler.wfile.write(body)


def backend_send_chunked(
    handler: BaseHTTPRequestHandler,
    chunks: list[bytes],
    content_type: str = "application/octet-stream",
) -> None:
    handler.wfile.write(b"HTTP/1.1 200 OK\r\n")
    handler.wfile.write(b"Transfer-Encoding: chunked\r\n")
    handler.wfile.write(f"Content-Type: {content_type}\r\n".encode())
    handler.wfile.write(b"\r\n")
    for chunk in chunks:
        handler.wfile.write(f"{len(chunk):X}\r\n".encode())
        handler.wfile.write(chunk)
        handler.wfile.write(b"\r\n")
    handler.wfile.write(b"0\r\n\r\n")
    handler.wfile.flush()
    handler.close_connection = True


def backend_send_eof(handler: BaseHTTPRequestHandler, body: bytes) -> None:
    handler.wfile.write(b"HTTP/1.1 200 OK\r\n")
    handler.wfile.write(b"Content-Type: application/octet-stream\r\n")
    handler.wfile.write(b"\r\n")
    handler.wfile.write(body)
    handler.wfile.flush()
    handler.close_connection = True


def test_routing_and_boundary_fallback(exe: str) -> None:
    backend_a = BackendServer()
    backend_b = BackendServer()
    backend_a.start()
    backend_b.start()
    try:
        marker_a = b"backend-a"
        marker_b = b"backend-b"

        def handle_a(handler: BaseHTTPRequestHandler) -> None:
            backend_send(handler, 200, marker_a, "text/plain")

        def handle_b(handler: BaseHTTPRequestHandler) -> None:
            backend_send(handler, 200, marker_b, "text/plain")

        backend_a.server.handlers = {
            ("GET", "/api/foo"): handle_a,
            ("GET", "/api/"): handle_a,
            ("GET", "/api/admin"): handle_a,
            ("GET", "/api/administer/"): handle_a,
        }
        backend_b.server.handlers = {
            ("GET", "/api/admin/foo"): handle_b,
            ("GET", "/api/admin/"): handle_b,
        }
        routes = [
            ("/api/", f"127.0.0.1:{backend_a.port}"),
            ("/api/admin/", f"127.0.0.1:{backend_b.port}"),
        ]
        proc, drain, _, port = start_server(exe, routes)
        try:
            baseline = fd_count(proc.pid)

            r = proxy_request(port, b"GET /api/foo HTTP/1.1\r\nHost: client\r\n\r\n")
            assert r.status == 200 and r.body == marker_a, r.body

            r = proxy_request(port, b"GET /api/admin/foo HTTP/1.1\r\nHost: client\r\n\r\n")
            assert r.status == 200 and r.body == marker_b, r.body

            curl = subprocess.run(
                ["curl", "--noproxy", "*", "--fail", "--silent", "--show-error",
                 "--max-time", "5", f"http://{HOST}:{port}/api/admin/foo"],
                capture_output=True, check=True, timeout=6,
            )
            assert curl.stdout == marker_b, curl.stdout

            r = proxy_request(port, b"GET /api/ HTTP/1.1\r\nHost: client\r\n\r\n")
            assert r.status == 200 and r.body == marker_a, r.body

            for path in (b"/api/admin", b"/api/administer/"):
                r = proxy_request(
                    port, b"GET " + path + b" HTTP/1.1\r\nHost: client\r\n\r\n"
                )
                assert r.status == 200 and r.body == marker_a, (path, r.status, r.body)

            for path in (b"/api", b"/apiadmin/"):
                r = proxy_request(
                    port,
                    b"GET " + path + b" HTTP/1.1\r\nHost: client\r\n\r\n",
                )
                assert r.status == 404, (path, r.status, r.body)
                assert r.body == b"Not Found", (path, r.body)

            wait_for_fd_count(proc.pid, baseline)
        finally:
            stop_server(proc, drain)
    finally:
        backend_a.stop()
        backend_b.stop()
    print("PASS ROUTING_AND_BOUNDARY_FALLBACK")


def test_round_robin_per_prefix(exe: str) -> None:
    """Overlapping routes select independent pools and wrap in configured order."""
    a = BackendServer()
    b = BackendServer()
    a.start()
    b.start()
    try:
        for path in ("/api/item", "/api/admin/item"):
            a.server.handlers[("GET", path)] = lambda h: backend_send(h, 200, b"A")
            b.server.handlers[("GET", path)] = lambda h: backend_send(h, 200, b"B")
        routes = [
            ("/api/", f"{HOST}:{a.port}"),
            ("/api/admin/", f"{HOST}:{b.port}"),
            ("/api/", f"{HOST}:{b.port}"),
            ("/api/admin/", f"{HOST}:{a.port}"),
        ]
        proc, drain, _, port = start_server(exe, routes)
        try:
            for broad, narrow in ((b"A", b"B"), (b"B", b"A"), (b"A", b"B")):
                for path, body in (("/api/item", broad), ("/api/admin/item", narrow)):
                    response = proxy_request(
                        port, f"GET {path} HTTP/1.1\r\nHost: client\r\n\r\n".encode()
                    )
                    assert (response.status, response.body) == (200, body), (
                        path, response.status, response.body
                    )
        finally:
            stop_server(proc, drain)
    finally:
        a.stop()
        b.stop()
    print("PASS ROUND_ROBIN_PER_PREFIX")


def test_backend_host_and_binary_post(exe: str) -> None:
    backend = BackendServer()
    backend.start()
    try:
        captured: dict[str, object] = {}

        def handle_echo(handler: BaseHTTPRequestHandler) -> None:
            captured["host"] = handler.headers.get("Host")
            captured["method"] = handler.command
            captured["path"] = handler.path
            body = _read_body(handler)
            captured["body"] = body
            backend_send(handler, 200, body, "application/octet-stream")

        backend.server.handlers = {("POST", "/api/echo"): handle_echo}
        routes = [("/api/", f"127.0.0.1:{backend.port}")]
        proc, drain, _, port = start_server(exe, routes)
        try:
            body = b"\x00\x01\x02\xff\xfe\xfd"
            req = (
                b"POST /api/echo?q=1&b=x HTTP/1.1\r\n"
                b"Host: client.example\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                b"\r\n" + body
            )
            r = proxy_request(port, req)
            assert r.status == 200, r.status
            assert r.headers.get("Content-Type") == "application/octet-stream"
            assert r.body == body, (r.body, body)
            assert captured["method"] == "POST"
            assert captured["path"] == "/api/echo?q=1&b=x"
            assert captured["host"] == f"127.0.0.1:{backend.port}"
        finally:
            stop_server(proc, drain)
    finally:
        backend.stop()
    print("PASS BACKEND_HOST_AND_BINARY_POST")


def test_backend_status_type_bytes(exe: str) -> None:
    backend = BackendServer()
    backend.start()
    try:
        def handle_create(handler: BaseHTTPRequestHandler) -> None:
            backend_send(
                handler,
                201,
                b"created",
                "text/plain",
                [("X-Custom", "yes")],
            )

        backend.server.handlers = {("POST", "/api/create"): handle_create}
        routes = [("/api/", f"127.0.0.1:{backend.port}")]
        proc, drain, _, port = start_server(exe, routes)
        try:
            r = proxy_request(
                port,
                b"POST /api/create HTTP/1.1\r\n"
                b"Host: client\r\n"
                b"Content-Length: 0\r\n\r\n",
            )
            assert r.status == 201, r.status
            assert r.body == b"created", r.body
            assert r.headers.get("Content-Type") == "text/plain"
            assert r.headers.get("X-Custom") == "yes"
        finally:
            stop_server(proc, drain)
    finally:
        backend.stop()
    print("PASS BACKEND_STATUS_TYPE_BYTES")


def test_upstream_gated_other_request_completes(exe: str) -> None:
    backend = BackendServer()
    backend.start()
    try:
        backend_holding = threading.Event()
        release = threading.Event()

        def handle_delay(handler: BaseHTTPRequestHandler) -> None:
            backend_holding.set()
            release.wait(timeout=10)
            backend_send(handler, 200, b"delayed", "text/plain")

        backend.server.handlers = {
            ("GET", "/api/delay"): handle_delay,
            ("GET", "/api/fast"): lambda h: backend_send(h, 200, b"independent"),
        }
        routes = [("/api/", f"127.0.0.1:{backend.port}")]
        proc, drain, _, port = start_server(exe, routes)
        try:
            a_result: dict[str, object] = {}
            a_done = threading.Event()

            def client_a() -> None:
                try:
                    r = proxy_request(
                        port,
                        b"GET /api/delay HTTP/1.1\r\nHost: client\r\n\r\n",
                        timeout=12,
                    )
                    a_result["status"] = r.status
                    a_result["body"] = r.body
                finally:
                    a_done.set()

            ta = threading.Thread(target=client_a, daemon=True)
            ta.start()
            assert backend_holding.wait(timeout=2)

            r = proxy_request(
                port, b"GET /api/fast HTTP/1.1\r\nHost: client\r\n\r\n", timeout=2
            )
            assert (r.status, r.body) == (200, b"independent")
            assert not a_done.is_set(), "delayed upstream finished before independent GET"

            release.set()
            a_done.wait(timeout=10)
            assert a_result.get("status") == 200 and a_result.get("body") == b"delayed"
            ta.join(timeout=5)
        finally:
            release.set()
            stop_server(proc, drain)
    finally:
        backend.stop()
    print("PASS UPSTREAM_GATED_OTHER_REQUEST_COMPLETES")


def test_large_stream_starts_before_backend_finishes(exe: str) -> None:
    backend = BackendServer()
    backend.start()
    try:
        first = b"A" * 1024
        second = b"B" * (1024 * 1024 - len(first))
        started = threading.Event()
        cont = threading.Event()

        def handle_stream(handler: BaseHTTPRequestHandler) -> None:
            handler.wfile.write(b"HTTP/1.1 200 OK\r\n")
            handler.wfile.write(b"Transfer-Encoding: chunked\r\n")
            handler.wfile.write(b"Content-Type: application/octet-stream\r\n")
            handler.wfile.write(b"\r\n")
            handler.wfile.write(f"{len(first):X}\r\n".encode())
            handler.wfile.write(first)
            handler.wfile.write(b"\r\n")
            handler.wfile.flush()
            started.set()
            cont.wait(timeout=10)
            chunk_size = 65536
            for i in range(0, len(second), chunk_size):
                chunk = second[i : i + chunk_size]
                handler.wfile.write(f"{len(chunk):X}\r\n".encode())
                handler.wfile.write(chunk)
                handler.wfile.write(b"\r\n")
                handler.wfile.flush()
            handler.wfile.write(b"0\r\n\r\n")
            handler.wfile.flush()
            handler.close_connection = True

        backend.server.handlers = {("GET", "/api/stream"): handle_stream}
        routes = [("/api/", f"127.0.0.1:{backend.port}")]
        proc, drain, _, port = start_server(exe, routes)
        try:
            with socket.create_connection((HOST, port), timeout=REQUEST_TIMEOUT) as sock:
                sock.sendall(b"GET /api/stream HTTP/1.1\r\nHost: client\r\n\r\n")
                with http.client.HTTPResponse(sock, method="GET") as resp:
                    resp.begin()
                    assert started.wait(timeout=2)
                    first_recv = resp.read(1024)
                    assert first_recv == first, (len(first_recv), len(first))
                    cont.set()
                    rest = resp.read()
                    assert len(rest) == len(second), (len(rest), len(second))
                    assert rest == second
        finally:
            cont.set()
            stop_server(proc, drain)
    finally:
        backend.stop()
    print("PASS LARGE_STREAM_STARTS_BEFORE_BACKEND_FINISHES")


def test_upstream_framing_variants(exe: str) -> None:
    backend = BackendServer()
    backend.start()
    try:
        def handle_head(handler: BaseHTTPRequestHandler) -> None:
            handler.send_response(200)
            handler.send_header("Content-Length", "123")
            handler.end_headers()

        backend.server.handlers = {
            ("GET", "/api/length"): lambda h: backend_send(h, 200, b"fixed", "text/plain"),
            ("GET", "/api/chunked"): lambda h: backend_send_chunked(h, [b"chu", b"nked"]),
            ("GET", "/api/eof"): lambda h: backend_send_eof(h, b"eof-body"),
            ("HEAD", "/api/length"): handle_head,
        }
        routes = [("/api/", f"127.0.0.1:{backend.port}")]
        proc, drain, _, port = start_server(exe, routes)
        try:
            for path, expected in (
                ("/api/length", b"fixed"),
                ("/api/chunked", b"chunked"),
                ("/api/eof", b"eof-body"),
            ):
                r = proxy_request(
                    port,
                    f"GET {path} HTTP/1.1\r\nHost: client\r\n\r\n".encode(),
                )
                assert r.status == 200, (path, r.status)
                assert r.body == expected, (path, r.body)
            r = proxy_request(port, b"HEAD /api/length HTTP/1.1\r\nHost: client\r\n\r\n")
            assert r.status == 200 and r.body == b"" and r.headers["Content-Length"] == "123"
        finally:
            stop_server(proc, drain)
    finally:
        backend.stop()
    print("PASS UPSTREAM_FRAMING_VARIANTS")


def test_timeout_and_refusal(exe: str) -> None:
    backend = BackendServer()
    backend.start()
    try:
        hang_release = threading.Event()

        def handle_hang(handler: BaseHTTPRequestHandler) -> None:
            hang_release.wait(timeout=30)

        backend.server.handlers = {("GET", "/api/hang/"): handle_hang}
        refuse_port = find_free_port()
        routes = [
            ("/api/hang/", f"127.0.0.1:{backend.port}"),
            ("/api/refuse/", f"127.0.0.1:{refuse_port}"),
        ]
        proc, drain, _, port = start_server(exe, routes)
        try:
            start = time.monotonic()
            r = proxy_request(
                port,
                b"GET /api/refuse/ HTTP/1.1\r\nHost: client\r\n\r\n",
                timeout=7,
            )
            elapsed = time.monotonic() - start
            assert r.status == 502, r.status
            assert elapsed < 3.0, elapsed

            start = time.monotonic()
            r = proxy_request(
                port,
                b"GET /api/hang/ HTTP/1.1\r\nHost: client\r\n\r\n",
                timeout=7,
            )
            elapsed = time.monotonic() - start
            assert r.status == 504, r.status
            assert 1.0 < elapsed < 7.0, elapsed
        finally:
            hang_release.set()
            stop_server(proc, drain)
    finally:
        backend.stop()
    print("PASS TIMEOUT_AND_REFUSAL")


def test_disconnects_and_fd_recovery(exe: str) -> None:
    backend = BackendServer()
    backend.start()
    try:
        backend_holding = threading.Event()
        release = threading.Event()

        def handle_hold(handler: BaseHTTPRequestHandler) -> None:
            backend_holding.set()
            release.wait(timeout=5)
            try:
                backend_send(handler, 200, b"ok", "text/plain")
            except Exception:
                pass

        backend.server.handlers = {("GET", "/api/hold"): handle_hold}
        routes = [("/api/", f"127.0.0.1:{backend.port}")]
        proc, drain, _, port = start_server(exe, routes)
        try:
            baseline = fd_count(proc.pid)

            with socket.create_connection((HOST, port), timeout=CONNECT_TIMEOUT) as client:
                client.sendall(b"GET /api/hold HTTP/1.1\r\nHost: client\r\n\r\n")
                assert backend_holding.wait(timeout=2)
                send_rst(client)

            release.set()
            wait_for_fd_count(proc.pid, baseline, timeout=3.0)

            baseline = fd_count(proc.pid)

            def handle_close(handler: BaseHTTPRequestHandler) -> None:
                handler.close_connection = True

            backend.server.handlers = {("GET", "/api/close"): handle_close}
            r = proxy_request(
                port,
                b"GET /api/close HTTP/1.1\r\nHost: client\r\n\r\n",
                timeout=5,
            )
            assert r.status == 502, r.status
            wait_for_fd_count(proc.pid, baseline, timeout=3.0)

            def handle_short(handler: BaseHTTPRequestHandler) -> None:
                handler.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nabc")
                handler.close_connection = True

            backend.server.handlers = {("GET", "/api/short"): handle_short}
            with socket.create_connection((HOST, port), timeout=REQUEST_TIMEOUT) as client:
                client.sendall(b"GET /api/short HTTP/1.1\r\nHost: client\r\n\r\n")
                with http.client.HTTPResponse(client) as response:
                    response.begin()
                    assert response.status == 200
                    try:
                        response.read()
                    except http.client.IncompleteRead as error:
                        assert error.partial == b"abc", error.partial
                    else:
                        raise AssertionError("short upstream response looked complete")
            wait_for_fd_count(proc.pid, baseline, timeout=3.0)
        finally:
            release.set()
            stop_server(proc, drain)
    finally:
        backend.stop()
    print("PASS DISCONNECTS_AND_FD_RECOVERY")


def test_graceful_shutdown(exe: str) -> None:
    backend = BackendServer()
    backend.start()
    try:
        backend_holding = threading.Event()
        release = threading.Event()

        def handle_hold(handler: BaseHTTPRequestHandler) -> None:
            backend_holding.set()
            release.wait(timeout=10)
            backend_send(handler, 200, b"finished", "text/plain")

        backend.server.handlers = {("GET", "/api/hold"): handle_hold}
        routes = [("/api/", f"127.0.0.1:{backend.port}")]
        proc, drain, _, port = start_server(exe, routes)
        try:
            result: dict[str, object] = {}
            done = threading.Event()

            def client() -> None:
                try:
                    r = proxy_request(
                        port,
                        b"GET /api/hold HTTP/1.1\r\nHost: client\r\n\r\n",
                        timeout=10,
                    )
                    result["status"] = r.status
                    result["body"] = r.body
                finally:
                    done.set()

            t = threading.Thread(target=client, daemon=True)
            t.start()
            assert backend_holding.wait(timeout=2)
            proc.terminate()
            release.set()
            assert done.wait(timeout=7)
            assert result.get("status") == 200 and result.get("body") == b"finished"

            late = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            late.settimeout(1.0)
            try:
                late.connect((HOST, port))
            except (socket.timeout, ConnectionRefusedError):
                pass
            else:
                late.settimeout(1.0)
                try:
                    data = late.recv(1024)
                except socket.timeout:
                    data = b""
                assert data == b"", f"late connection got response after shutdown: {data!r}"
                late.close()
            t.join(timeout=5)
        finally:
            release.set()
            stop_server(proc, drain)
    finally:
        backend.stop()
    print("PASS GRACEFUL_SHUTDOWN")


def main() -> None:
    if len(sys.argv) != 2:
        print("usage: test_proxy.py <path-to-reverse-proxy-executable>", file=sys.stderr)
        sys.exit(2)
    exe = os.path.abspath(sys.argv[1])
    tests = [
        test_routing_and_boundary_fallback,
        test_round_robin_per_prefix,
        test_backend_host_and_binary_post,
        test_backend_status_type_bytes,
        test_upstream_gated_other_request_completes,
        test_large_stream_starts_before_backend_finishes,
        test_upstream_framing_variants,
        test_timeout_and_refusal,
        test_disconnects_and_fd_recovery,
        test_graceful_shutdown,
    ]
    for test in tests:
        test(exe)
    print("PROXY_TESTS_PASS")


if __name__ == "__main__":
    main()
