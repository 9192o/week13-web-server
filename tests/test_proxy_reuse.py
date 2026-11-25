"""Loopback regressions for independent downstream and upstream reuse."""

import http.client
import socket
import sys
from contextlib import closing
from http.server import BaseHTTPRequestHandler

from test_proxy import (
    BackendServer,
    HOST,
    backend_send,
    find_free_port,
    proxy_request,
    start_server,
    stop_server,
)

GET_CLOSE = b"GET /check HTTP/1.1\r\nHost: client\r\nConnection: close\r\n\r\n"
POST = b"POST /check HTTP/1.1\r\nHost: client\r\nContent-Length: 3\r\n\r\nabc"


def downstream_reuse(exe: str, backend: BackendServer) -> None:
    proc, drain, _, port = start_server(exe, [("/", f"{HOST}:{backend.port}")])
    try:
        with closing(http.client.HTTPConnection(HOST, port, timeout=3)) as client:
            client.request("GET", "/check")
            first_socket = client.sock
            with client.getresponse() as response:
                assert (response.status, response.read()) == (200, b"ok")
            assert client.sock is first_socket, "proxy closed the first client socket"
            client.request("GET", "/check", headers={"Connection": "close"})
            assert client.sock is first_socket
            with client.getresponse() as response:
                assert (response.status, response.read()) == (200, b"ok")
            assert client.sock is None, "client close request ignored"
    finally:
        stop_server(proc, drain)


def upstream_reuse(exe: str, backend: BackendServer) -> None:
    seen: list[int] = []

    def record(handler: BaseHTTPRequestHandler) -> None:
        seen.append(handler.client_address[1])
        backend_send(handler, 200, b"ok")

    backend.server.handlers[("GET", "/check")] = record
    proc, drain, _, port = start_server(exe, [("/", f"{HOST}:{backend.port}")])
    try:
        assert [proxy_request(port, GET_CLOSE).status for _ in range(3)] == [200] * 3
        assert len(seen) == 3
        assert len(set(seen)) == 1, f"new upstream connection per request: {seen}"
    finally:
        stop_server(proc, drain)


def retry_only_connect_failure(exe: str, backend: BackendServer) -> None:
    seen: list[bytes] = []

    def post(handler: BaseHTTPRequestHandler) -> None:
        seen.append(handler.rfile.read(3))
        backend_send(handler, 200, b"ok")

    backend.server.handlers[("POST", "/check")] = post
    routes = [("/", f"{HOST}:{find_free_port()}"), ("/", f"{HOST}:{backend.port}")]
    proc, drain, _, port = start_server(exe, routes)
    try:
        result = proxy_request(port, POST)
        assert (result.status, result.body) == (200, b"ok"), result.status
        assert seen == [b"abc"]
    finally:
        stop_server(proc, drain)

    first = BackendServer()
    first.start()
    delivered = []

    def close_after_post(handler: BaseHTTPRequestHandler) -> None:
        delivered.append(handler.rfile.read(3))
        handler.connection.shutdown(socket.SHUT_RDWR)
        handler.close_connection = True

    first.server.handlers[("POST", "/check")] = close_after_post
    routes = [("/", f"{HOST}:{first.port}"), ("/", f"{HOST}:{backend.port}")]
    proc, drain, _, port = start_server(exe, routes)
    try:
        assert proxy_request(port, POST).status == 502
        assert delivered == [b"abc"] and seen == [b"abc"], "POST was replayed"
    finally:
        stop_server(proc, drain)
        first.stop()


if __name__ == "__main__":
    tests = (downstream_reuse, upstream_reuse, retry_only_connect_failure)
    for test in tests:
        if len(sys.argv) > 2 and test.__name__ not in sys.argv[2:]:
            continue
        backend = BackendServer()
        backend.server.handlers[("GET", "/check")] = lambda h: backend_send(h, 200, b"ok")
        backend.start()
        try:
            test(sys.argv[1], backend)
        finally:
            backend.stop()
        print(f"PASS {test.__name__}")
