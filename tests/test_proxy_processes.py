#!/usr/bin/env python3
"""Exercise the separately launched C proxy and C origin over real sockets."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

from test_proxy import HOST, find_free_port, proxy_request, start_process, stop_server


def test_backend_lifecycle(proxy: str, backend: str) -> None:
    """Restart the origin on the same port without restarting the proxy."""
    origin_port = find_free_port()
    proxy_port = find_free_port()
    while proxy_port == origin_port:
        proxy_port = find_free_port()
    route = f"/={HOST}:{origin_port}"
    get = b"GET / HTTP/1.1\r\nHost: client\r\nConnection: close\r\n\r\n"
    with tempfile.TemporaryDirectory() as root:
        index = Path(root) / "index.html"
        args = [backend, "--port", str(origin_port), "--root", root]
        proc, drain, _, _ = start_process(
            [proxy, "--port", str(proxy_port), "--route", route], proxy_port
        )
        try:
            assert proxy_request(proxy_port, get).status == 502
            rejected = subprocess.run(
                [*args, "--route", route], capture_output=True, timeout=5, check=False
            )
            assert rejected.returncode == 1, rejected
            for index_body in (b"original backend", b"restarted backend"):
                index.write_bytes(index_body)
                origin, origin_drain, _, _ = start_process(args, origin_port)
                try:
                    reply = proxy_request(proxy_port, get)
                    assert (reply.status, reply.body) == (200, index_body)
                    payload = b"echo\0through\xffproxy"
                    post = (
                        b"POST /echo HTTP/1.1\r\nHost: client\r\nContent-Length: "
                        + str(len(payload)).encode()
                        + b"\r\n\r\n"
                        + payload
                    )
                    reply = proxy_request(proxy_port, post)
                    assert (reply.status, reply.body) == (200, payload)
                finally:
                    stop_server(origin, origin_drain)
                assert proxy_request(proxy_port, get).status == 502
        finally:
            stop_server(proc, drain)
    print("PASS PROXY_AND_BACKEND_PROCESS_LIFECYCLE")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: test_proxy_processes.py <reverse-proxy> <webserver>")
    test_backend_lifecycle(os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]))
