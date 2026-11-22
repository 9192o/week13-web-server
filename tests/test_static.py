#!/usr/bin/env python3
"""Wire contracts for publishing files and directory indexes."""

import http.client
import os
import selectors
import socket
import subprocess
import sys
import tempfile
from pathlib import Path


def request(port: int, method: str, path: str) -> tuple[int, http.client.HTTPMessage, bytes]:
    with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
        client.sendall(f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode("ascii"))
        with http.client.HTTPResponse(client, method=method) as response:
            response.begin()
            return response.status, response.headers, response.read()


def check_static(port: int, payload: bytes) -> None:
    status, headers, body = request(port, "GET", "/%ED%95%9C%20file.CSS?v=2")
    assert (status, headers["Content-Type"], body) == (200, "text/css; charset=utf-8", payload)

    status, headers, body = request(port, "HEAD", "/assets?ignored=yes")
    assert (status, headers["Location"], body) == (308, "/assets/", b"")
    assert request(port, "GET", "/assets/")[2] == b"nested index"
    assert request(port, "GET", "/empty/")[0] == 404
    status, headers, _ = request(port, "POST", "/assets/large.png")
    assert (status, headers["Allow"]) == (405, "GET, HEAD")
    assert request(port, "POST", "/not-found.png")[0] == 404

    status, headers, body = request(port, "HEAD", "/assets/large.png")
    assert (status, headers["Content-Length"], body) == (200, str(len(payload)), b"")
    assert request(port, "GET", "/assets/large.png")[2] == payload

    for path in ("/assets/alias.png", "/absolute.png", "/directory/large.png"):
        assert request(port, "GET", path)[2] == payload, path
    for path, expected in (
        ("/private.txt", 403),
        ("/not-found.png", 404),
        ("/.hidden.png", 404),
        ("/hidden-alias.png", 404),
        ("/events.png", 403),
    ):
        assert request(port, "GET", path)[0] == expected, path


def check_curl(port: int) -> None:
    """Check the real curl client against the socket and binary echo."""
    curl = ["curl", "--silent", "--show-error", "--fail", "--noproxy", "*", "--max-time", "5"]
    base = f"http://127.0.0.1:{port}"
    get = subprocess.run([*curl, f"{base}/assets/"], capture_output=True, check=True)
    assert get.stdout == b"nested index"
    data = b"curl\0binary\xff\n"
    echo = subprocess.run(
        [*curl, "--data-binary", "@-", f"{base}/echo"],
        input=data,
        capture_output=True,
        check=True,
    )
    assert echo.stdout == data


def main() -> None:
    payload = bytes(range(256)) * 4100
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        (root / "assets").mkdir()
        (root / "empty").mkdir()
        (root / "assets/index.html").write_bytes(b"nested index")
        (root / "assets/large.png").write_bytes(payload)
        (root / "한 file.CSS").write_bytes(payload)
        (root / "private.txt").write_bytes(b"not a public extension")
        (root / ".hidden.png").touch()
        (root / "assets/alias.png").symlink_to("large.png")
        (root / "absolute.png").symlink_to(root / "assets/large.png")
        (root / "directory").symlink_to("assets", target_is_directory=True)
        (root / "hidden-alias.png").symlink_to(".hidden.png")
        os.mkfifo(root / "events.png")

        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        with subprocess.Popen(
            [str(exe), "--port", str(port), "--root", str(root)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        ) as proc:
            try:
                assert proc.stdout is not None
                with selectors.DefaultSelector() as events:
                    events.register(proc.stdout, selectors.EVENT_READ)
                    assert events.select(timeout=5), "server did not signal readiness"
                assert proc.stdout.readline() == f"READY {port}\n".encode("ascii")
                # This fixture emits fewer than 20 short access records.
                check_static(port, payload)
                check_curl(port)
            finally:
                if proc.poll() is None:
                    proc.terminate()
                proc.wait(timeout=5)
    print("STATIC_INTEGRATION_TESTS_PASS")


if __name__ == "__main__":
    main()
