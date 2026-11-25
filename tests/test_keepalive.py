#!/usr/bin/env python3

import json
import socket
import sys
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import BinaryIO, Final

from test_server import HOST, start_server, stop_server

INDEX: Final = b"phase two test index"
GET: Final = b"GET / HTTP/1.1\r\nHost: local\r\n\r\n"
MISS: Final = b"GET /missing HTTP/1.1\r\nHost: local\r\n\r\n"
CLOSE: Final = b"GET / HTTP/1.1\r\nHost: local\r\nConnection: keep-alive, ClOsE\r\n\r\n"


def expect_response(
    stream: BinaryIO, status: int, body: bytes, *, close: bool = False
) -> None:
    """Read exactly one framed response without taking its successor's bytes."""
    line = stream.readline()
    assert line.startswith(f"HTTP/1.1 {status} ".encode()), line
    headers: dict[bytes, bytes] = {}
    while (line := stream.readline()) != b"\r\n":
        assert line and b":" in line, line
        name, value = line.rstrip(b"\r\n").split(b":", 1)
        headers[name.lower()] = value.strip()
    assert int(headers[b"content-length"]) == len(body), headers
    assert (headers.get(b"connection", b"").lower() == b"close") == close, headers
    assert stream.read(len(body)) == body


@contextmanager
def channel(port: int) -> Iterator[tuple[socket.socket, BinaryIO]]:
    with socket.create_connection((HOST, port), timeout=8) as conn, conn.makefile(
        "rb"
    ) as stream:
        yield conn, stream


def check_pipeline(port: int) -> None:
    for framing in (
        b"Content-Length: 3\r\n\r\nx\x00y",
        b"Transfer-Encoding: chunked\r\n\r\n3\r\nx\x00y\r\n0\r\n\r\n",
    ):
        with channel(port) as (conn, stream):
            conn.sendall(b"POST /echo HTTP/1.1\r\nHost: local\r\n" + framing + MISS + CLOSE)
            expect_response(stream, 200, b"x\x00y")
            expect_response(stream, 404, b"Not Found")
            expect_response(stream, 200, INDEX, close=True)
            assert stream.read(1) == b""


def check_request_limit(port: int) -> None:
    with channel(port) as (conn, stream):
        conn.sendall(GET * 101)
        for number in range(1, 101):
            expect_response(stream, 200, INDEX, close=number == 100)
        assert stream.read(1) == b""


def check_incomplete_request_closes(port: int) -> None:
    with channel(port) as (conn, stream):
        conn.sendall(
            b"POST /missing HTTP/1.1\r\nHost: local\r\nContent-Length: 100\r\n\r\n" + GET
        )
        expect_response(stream, 404, b"Not Found", close=True)
        assert stream.read(1) == b""


def check_idle_and_access_logs(executable: str, root: str) -> None:
    proc, thread, output, port = start_server(executable, root)
    try:
        with channel(port) as (conn, stream):
            conn.sendall(GET)
            expect_response(stream, 200, INDEX)
            conn.sendall(MISS)
            expect_response(stream, 404, b"Not Found")
            assert stream.read(1) == b""
    finally:
        stop_server(proc, thread)
    records = []
    while (line := output.get_nowait()) is not None:
        records.append(json.loads(line))
    assert [(record["path"], record["status"]) for record in records] == [
        ("/", 200),
        ("/missing", 404),
    ], records


def main(executable: str) -> None:
    with TemporaryDirectory() as root:
        (Path(root) / "index.html").write_bytes(INDEX)
        proc, thread, _, port = start_server(executable, root)
        try:
            check_pipeline(port)
            check_request_limit(port)
            check_incomplete_request_closes(port)
        finally:
            stop_server(proc, thread)
        check_idle_and_access_logs(executable, root)
    print("KEEPALIVE_INTEGRATION_TESTS_PASS")


if __name__ == "__main__":
    main(sys.argv[1])
