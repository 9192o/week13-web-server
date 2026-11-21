#!/usr/bin/env python3

import http.client
import os
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from email.utils import parsedate_to_datetime
from typing import Final, Optional

HOST: Final = "127.0.0.1"
PORT: Final = 8080
READY_TIMEOUT: Final = 5.0
REQUEST_TIMEOUT: Final = 5.0
TERM_TIMEOUT: Final = 5.0

INDEX_PATH: Final = Path(__file__).parent.parent / "index.html"
INDEX_BYTES: Final = INDEX_PATH.read_bytes()
INDEX_LEN: Final = len(INDEX_BYTES)

REASON_PHRASES: Final = {
    400: b"Bad Request",
    403: b"Forbidden",
    404: b"Not Found",
    405: b"Method Not Allowed",
    413: b"Content Too Large",
    414: b"URI Too Long",
    417: b"Expectation Failed",
    431: b"Request Header Fields Too Large",
    501: b"Not Implemented",
    505: b"HTTP Version Not Supported",
}


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


def stop_server(
    proc: subprocess.Popen[bytes], drain_thread: threading.Thread
) -> None:
    proc.terminate()
    try:
        proc.wait(timeout=TERM_TIMEOUT)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=TERM_TIMEOUT)
    drain_thread.join(timeout=TERM_TIMEOUT)
    assert not drain_thread.is_alive()


def start_server(
    exe: str, cwd: Optional[str] = None
) -> tuple[subprocess.Popen[bytes], threading.Thread, queue.Queue[bytes | None]]:
    proc = subprocess.Popen(
        [exe],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        cwd=cwd,
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
            return proc, t, q

    stop_server(proc, t)
    raise AssertionError("server did not become ready")


def read_to_eof(sock: socket.socket) -> bytes:
    data = bytearray()
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        data.extend(chunk)
        assert len(data) <= 32768
    return bytes(data)


def raw_request(
    request: bytes,
    *,
    shutdown_write: bool = False,
    method: Optional[str] = None,
) -> Response:
    with socket.create_connection((HOST, PORT), timeout=REQUEST_TIMEOUT) as sock:
        if request:
            sock.sendall(request)
        if shutdown_write:
            sock.shutdown(socket.SHUT_WR)
        with http.client.HTTPResponse(sock, method=method) as response:
            response.begin()
            assert response.version == 11
            body = response.read()
            return Response(response.status, response.msg, body)


def raw_headers_only(
    request: bytes,
    *,
    shutdown_write: bool = False,
) -> tuple[bytes, bytes]:
    with socket.create_connection((HOST, PORT), timeout=REQUEST_TIMEOUT) as sock:
        if request:
            sock.sendall(request)
        if shutdown_write:
            sock.shutdown(socket.SHUT_WR)
        head = bytearray()
        while not head.endswith(b"\r\n\r\n"):
            byte = sock.recv(1)
            assert byte, "EOF before end of headers"
            head.extend(byte)
            assert len(head) <= 32768
        trailing = read_to_eof(sock)
        return bytes(head), trailing


def expect_status(response: Response, status: int) -> None:
    assert response.status == status, (response.status, status)


def expect_legacy_error(response: Response, status: int) -> None:
    expect_status(response, status)
    assert response.headers.get("Content-Length") == "0"
    assert response.headers.get("Connection") == "close"
    assert response.body == b""


def expect_routed_common(
    response: Response,
    status: int,
    content_type: str,
    body_len: int,
) -> None:
    expect_status(response, status)
    assert response.headers.get("Content-Type") == content_type
    assert response.headers.get("Content-Length") == str(body_len)
    assert response.headers.get("Connection") == "close"
    assert response.headers.get("X-Content-Type-Options") == "nosniff"
    date = response.headers.get("Date")
    assert date is not None
    assert parsedate_to_datetime(date).utcoffset().total_seconds() == 0


def expect_index(response: Response, head: bool = False) -> None:
    expect_routed_common(
        response,
        200,
        "text/html; charset=utf-8",
        INDEX_LEN,
    )
    if head:
        assert response.body == b""
    else:
        assert response.body == INDEX_BYTES


def expect_error_body(response: Response, status: int) -> None:
    phrase = REASON_PHRASES.get(status, b"")
    assert response.body == phrase, (response.body, phrase)


def expect_route_error(
    response: Response,
    status: int,
    allow: Optional[str] = None,
    *,
    head: bool = False,
) -> None:
    expect_routed_common(
        response,
        status,
        "text/plain; charset=utf-8",
        len(REASON_PHRASES[status]),
    )
    if not head:
        expect_error_body(response, status)
    else:
        assert response.body == b""
    if allow is not None:
        assert response.headers.get("Allow") == allow
    else:
        assert "Allow" not in response.headers


def expect_echo(response: Response, body: bytes) -> None:
    expect_routed_common(
        response,
        200,
        "application/octet-stream",
        len(body),
    )
    assert response.body == body


def expect_continue_then_final(
    request_head: bytes,
    body_wire: bytes,
    expected_status: int,
    expected_body: bytes = b"",
) -> None:
    with socket.create_connection((HOST, PORT), timeout=REQUEST_TIMEOUT) as sock:
        sock.sendall(request_head)
        head = bytearray()
        while not head.endswith(b"\r\n\r\n"):
            byte = sock.recv(1)
            assert byte
            head.extend(byte)
            assert len(head) <= 32768
        first = bytes(head)
        assert first == b"HTTP/1.1 100 Continue\r\n\r\n", first
        sock.sendall(body_wire)
        with http.client.HTTPResponse(sock) as response:
            response.begin()
            assert response.version == 11
            assert response.status == expected_status, response.status
            body = response.read()
            assert body == expected_body, (len(body), len(expected_body))


def expect_final_without_continue(
    request: bytes,
    expected_status: int,
    *,
    shutdown_write: bool = False,
) -> None:
    head, body = raw_headers_only(request, shutdown_write=shutdown_write)
    assert head.startswith(f"HTTP/1.1 {expected_status}".encode()), head
    assert not head.startswith(b"HTTP/1.1 100"), head
    cl = 0
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            cl = int(line.split(b":", 1)[1].strip())
            break
    assert body == REASON_PHRASES[expected_status][:cl]


def test_index_routes() -> None:
    expect_index(raw_request(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"))
    expect_index(raw_request(b"GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n"))
    expect_index(raw_request(b"GET /?ignored=1 HTTP/1.1\r\nHost: x\r\n\r\n"))
    expect_index(raw_request(b"GET /index.html?ignored=2 HTTP/1.1\r\nHost: x\r\n\r\n"))
    expect_index(raw_request(b"GET / HTTP/1.9\r\nHost: x\r\n\r\n"))
    expect_index(
        raw_request(b"GET http://example.org/index.html?x=1 HTTP/1.1\r\nHost: other\r\n\r\n")
    )


def test_head_routes() -> None:
    expect_index(raw_request(b"HEAD / HTTP/1.1\r\nHost: x\r\n\r\n", method="HEAD"), head=True)
    expect_index(raw_request(b"HEAD /index.html HTTP/1.1\r\nHost: x\r\n\r\n", method="HEAD"), head=True)
    head, trailing = raw_headers_only(b"HEAD / HTTP/1.1\r\nHost: x\r\n\r\n")
    assert head.startswith(b"HTTP/1.1 200")
    assert trailing == b""
    head, trailing = raw_headers_only(b"HEAD /notfound HTTP/1.1\r\nHost: x\r\n\r\n")
    assert head.startswith(b"HTTP/1.1 404")
    assert trailing == b""


def test_method_errors() -> None:
    expect_route_error(raw_request(b"POST / HTTP/1.1\r\nHost: x\r\n\r\n"), 405, "GET, HEAD")
    expect_route_error(
        raw_request(b"POST /index.html HTTP/1.1\r\nHost: x\r\n\r\n"), 405, "GET, HEAD"
    )
    expect_route_error(raw_request(b"GET /echo HTTP/1.1\r\nHost: x\r\n\r\n"), 405, "POST")
    expect_route_error(raw_request(b"HEAD /echo HTTP/1.1\r\nHost: x\r\n\r\n", method="HEAD"), 405, "POST", head=True)
    expect_route_error(raw_request(b"DELETE / HTTP/1.1\r\nHost: x\r\n\r\n"), 501)
    expect_route_error(
        raw_request(b"DELETE /index.html HTTP/1.1\r\nHost: x\r\n\r\n"), 501
    )
    expect_route_error(raw_request(b"PUT /echo HTTP/1.1\r\nHost: x\r\n\r\n"), 501)
    expect_route_error(raw_request(b"CUSTOM / HTTP/1.1\r\nHost: x\r\n\r\n"), 501)


def test_unknown_paths() -> None:
    expect_route_error(raw_request(b"GET /unknown HTTP/1.1\r\nHost: x\r\n\r\n"), 404)
    expect_route_error(raw_request(b"GET /foo HTTP/1.1\r\nHost: x\r\n\r\n"), 404)
    expect_route_error(raw_request(b"GET /ok HTTP/1.1\r\nHost: x\r\n\r\n"), 404)
    expect_route_error(
        raw_request(b"GET http://example.org/a?b HTTP/1.1\r\nHost: other\r\n\r\n"), 404
    )


def test_percent_decoding_and_traversal() -> None:
    expect_index(raw_request(b"GET /index%2ehtml HTTP/1.1\r\nHost: x\r\n\r\n"))
    expect_route_error(
        raw_request(b"GET /%2e%2e/index.html HTTP/1.1\r\nHost: x\r\n\r\n"), 404
    )
    expect_route_error(
        raw_request(b"GET /..%2findex.html HTTP/1.1\r\nHost: x\r\n\r\n"), 404
    )
    expect_legacy_error(
        raw_request(b"GET /index%00.html HTTP/1.1\r\nHost: x\r\n\r\n"), 400
    )
    expect_legacy_error(
        raw_request(b"GET /index%ZZ HTTP/1.1\r\nHost: x\r\n\r\n"), 400
    )
    expect_legacy_error(
        raw_request(b"GET /index%2 HTTP/1.1\r\nHost: x\r\n\r\n"), 400
    )
    expect_echo(raw_request(b"POST /%65cho HTTP/1.1\r\nHost: x\r\n\r\n"), b"")
    expect_route_error(
        raw_request(b"POST /%2565cho HTTP/1.1\r\nHost: x\r\n\r\n"), 404
    )


def test_echo() -> None:
    body = b"a\0b\xffz"
    expect_echo(
        raw_request(
            b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n" + body
        ),
        body,
    )
    expect_echo(
        raw_request(
            b"POST /echo?ignored=1 HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: 5\r\n\r\n" + body
        ),
        body,
    )
    expect_echo(
        raw_request(
            b"POST /echo HTTP/1.1\r\nHost: x\r\n"
            b"Transfer-Encoding: chunked\r\n\r\n"
            b"3\r\na\0b\r\n2\r\n\xffz\r\n0\r\n\r\n"
        ),
        body,
    )
    expect_echo(
        raw_request(b"POST /echo HTTP/1.1\r\nHost: x\r\n\r\n"),
        b"",
    )
    injection = (
        b"<script>alert(1)</script>\0\xff\r\n"
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Length: 0\r\n"
        b"X-Injected: yes\r\n"
        b"\r\n"
    )
    reflected = raw_request(
        b"POST /echo HTTP/1.1\r\nHost: x\r\n"
        b"Content-Type: text/html\r\nContent-Encoding: gzip\r\nX-Injected: yes\r\n"
        b"Content-Length: " + str(len(injection)).encode() + b"\r\n\r\n" + injection
    )
    expect_echo(reflected, injection)
    assert reflected.headers.get("Content-Encoding") is None
    assert reflected.headers.get("X-Injected") is None
    maximum = bytes(range(256)) * 4096
    assert len(maximum) == 1048576
    expect_echo(
        raw_request(
            b"POST /echo HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: 1048576\r\n\r\n" + maximum
        ),
        maximum,
    )
    expect_echo(
        raw_request(
            b"POST /echo HTTP/1.1\r\nHost: x\r\n"
            b"Transfer-Encoding: chunked\r\n\r\n"
            b"100000\r\n" + maximum + b"\r\n0\r\n\r\n"
        ),
        maximum,
    )
    expect_continue_then_final(
        b"POST /echo HTTP/1.1\r\nHost: x\r\n"
        b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n",
        body,
        200,
        body,
    )
    expect_continue_then_final(
        b"POST /echo HTTP/1.1\r\nHost: x\r\n"
        b"Transfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n",
        b"3\r\na\0b\r\n2\r\n\xffz\r\n0\r\n\r\n",
        200,
        body,
    )


def test_get_head_with_body() -> None:
    expect_index(
        raw_request(
            b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
        )
    )
    expect_index(
        raw_request(
            b"GET / HTTP/1.1\r\nHost: x\r\n"
            b"Transfer-Encoding: chunked\r\n\r\n"
            b"5\r\nhello\r\n0\r\n\r\n"
        )
    )
    expect_index(
        raw_request(
            b"HEAD / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello",
            method="HEAD",
        ),
        head=True,
    )


def test_legacy_parser_framing_errors() -> None:
    expect_legacy_error(raw_request(b"GET / HTTP/1.1\r\n\r\n"), 400)
    expect_legacy_error(
        raw_request(b"GET / HTTP/1.1\r\nHost: x\r\nHOST: x\r\n\r\n"), 400
    )
    expect_legacy_error(
        raw_request(b"GET / HTTP/1.1\r\nHost: [bad]\r\n\r\n"), 400
    )
    expect_legacy_error(raw_request(b"GET / HTTP/1.0\r\nHost: x\r\n\r\n"), 505)
    expect_legacy_error(raw_request(b"GET / HTTP/2.0\r\nHost: x\r\n\r\n"), 505)
    expect_legacy_error(raw_request(b"GET / HTTP/1.1 extra\r\n\r\n"), 400)

    target_len = 8192 - len(b"GET /") - len(b" HTTP/1.1\r\n")
    exact = b"GET /" + b"A" * target_len + b" HTTP/1.1\r\n"
    assert len(exact) == 8192
    expect_route_error(raw_request(exact + b"Host: x\r\n\r\n"), 404)

    too_long = b"GET /" + b"A" * (target_len + 1) + b" HTTP/1.1\r\n"
    assert len(too_long) == 8193
    expect_legacy_error(raw_request(too_long), 414)
    expect_legacy_error(raw_request(b"GET / HTTP/1.1\r", shutdown_write=True), 400)
    expect_legacy_error(raw_request(b"", shutdown_write=True), 400)
    expect_legacy_error(
        raw_request(b"GET / HTTP/1.1\r\nHost: x", shutdown_write=True), 400
    )

    prefix = b"POST /echo HTTP/1.1\r\nHost: x\r\n"
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
        expect_legacy_error(raw_request(prefix + fields + b"\r\n\r\n"), status)
    expect_legacy_error(raw_request(prefix + b"X: a\r\n" * 101 + b"\r\n"), 431)
    expect_legacy_error(
        raw_request(prefix + b"Content-Length: 5\r\n\r\nab", shutdown_write=True), 400
    )
    expect_legacy_error(
        raw_request(
            prefix + b"Transfer-Encoding: chunked\r\n\r\n5\r\nab",
            shutdown_write=True,
        ),
        400,
    )
    expect_legacy_error(
        raw_request(
            prefix + b"Transfer-Encoding: chunked\r\n\r\n0\r\nX: a\r",
            shutdown_write=True,
        ),
        400,
    )
    expect_legacy_error(
        raw_request(prefix + b"Transfer-Encoding: chunked\r\n\r\n100001\r\n"), 413
    )


def test_expect_route_errors_before_100() -> None:
    for request, status in [
        (
            b"POST /echo HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n",
            400,
        ),
        (
            b"POST /bad% HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n",
            400,
        ),
        (
            b"POST /echo HTTP/1.0\r\nHost: x\r\n"
            b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n",
            505,
        ),
        (
            b"POST /echo HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: 1048577\r\nExpect: 100-continue\r\n\r\n",
            413,
        ),
    ]:
        expect_final_without_continue(request, status)
    expect_final_without_continue(
        b"POST /unknown HTTP/1.1\r\nHost: x\r\n"
        b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n",
        404,
    )
    expect_final_without_continue(
        b"POST / HTTP/1.1\r\nHost: x\r\n"
        b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n",
        405,
    )
    expect_final_without_continue(
        b"POST /index.html HTTP/1.1\r\nHost: x\r\n"
        b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n",
        405,
    )
    expect_final_without_continue(
        b"GET /echo HTTP/1.1\r\nHost: x\r\n"
        b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n",
        405,
    )
    expect_final_without_continue(
        b"POST /unknown HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n",
        404,
    )
    expect_final_without_continue(
        b"POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n",
        405,
    )


def test_file_errors_before_body(index_path: Path) -> None:
    backup = index_path.with_name("index.html.bak")
    try:
        index_path.rename(backup)
        expect_final_without_continue(
            b"GET / HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n",
            404,
        )
        expect_route_error(raw_request(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"), 404)
    finally:
        backup.rename(index_path)

    index_path.rename(backup)
    try:
        os.symlink(str(backup), str(index_path))
        expect_final_without_continue(
            b"GET / HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n",
            403,
        )
        expect_route_error(raw_request(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"), 403)
    finally:
        index_path.unlink()
        backup.rename(index_path)

    index_path.rename(backup)
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as file_socket:
            file_socket.bind(str(index_path))
            expect_final_without_continue(
                b"GET / HTTP/1.1\r\nHost: x\r\n"
                b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n",
                403,
            )
            expect_route_error(raw_request(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"), 403)
    finally:
        index_path.unlink()
        backup.rename(index_path)

    index_path.rename(backup)
    try:
        os.mkfifo(str(index_path))
        expect_final_without_continue(
            b"GET / HTTP/1.1\r\nHost: x\r\n"
            b"Content-Length: 5\r\nExpect: 100-continue\r\n\r\n",
            403,
        )
        expect_route_error(raw_request(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"), 403)
    finally:
        index_path.unlink()
        backup.rename(index_path)


def main() -> None:
    if len(sys.argv) != 2:
        print("usage: test_server.py <path-to-webserver-executable>", file=sys.stderr)
        sys.exit(2)

    exe = os.path.abspath(sys.argv[1])

    with tempfile.TemporaryDirectory(prefix="webserver-routes-") as directory:
        index_path = Path(directory) / "index.html"
        index_path.write_bytes(INDEX_BYTES)
        proc, drain, _ = start_server(exe, cwd=directory)
        try:
            test_index_routes()
            test_head_routes()
            test_method_errors()
            test_unknown_paths()
            test_percent_decoding_and_traversal()
            test_get_head_with_body()
            test_legacy_parser_framing_errors()
            test_expect_route_errors_before_100()
            test_echo()
            test_file_errors_before_body(index_path)
        finally:
            stop_server(proc, drain)

    print("INTEGRATION_TESTS_PASS")


if __name__ == "__main__":
    main()
