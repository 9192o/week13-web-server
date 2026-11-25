"""CGI HTTP behavior with trusted fixtures in an isolated temporary root."""

import concurrent.futures
import os
import sys
import tempfile
import time
from pathlib import Path

from test_proxy import find_free_port, proxy_request, start_process, stop_server


def main(exe: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        scripts = root / "scripts"
        scripts.mkdir()
        (root / "index.html").write_bytes(b"static")
        started = root / "started"
        for name, content in (
            ("echo", "printf 'Content-Type: application/octet-stream\\n\\n'; cat"),
            ("slow", f"printf x > '{started}'; exec 1>&-; exec sleep 30"),
        ):
            script = scripts / name
            script.write_text("#!/bin/sh\n" + content + "\n")
            script.chmod(0o700)
        port = find_free_port()
        proc, drain, _, _ = start_process(
            [exe, "--port", str(port), "--root", tmp, "--cgi-root", str(scripts)], port
        )
        try:
            body = b"x\0\r\n\r\ny"
            result = proxy_request(
                port, b"POST /cgi-bin/echo HTTP/1.1\r\nHost: test\r\n"
                b"Content-Length: 7\r\n\r\n" + body,
            )
            assert (result.status, result.body) == (200, body)
            with concurrent.futures.ThreadPoolExecutor(1) as executor:
                slow = executor.submit(
                    proxy_request, port, b"GET /cgi-bin/slow HTTP/1.1\r\nHost: test\r\n\r\n"
                )
                deadline = time.monotonic() + 3
                while not started.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                assert started.exists(), "CGI fixture did not start"
                result = proxy_request(port, b"GET / HTTP/1.1\r\nHost: test\r\n\r\n", 1)
                assert (result.status, result.body) == (200, b"static")
                assert slow.result(timeout=7).status == 504
            child_paths = Path(f"/proc/{proc.pid}/task").glob("*/children")
            assert all(not path.read_text().strip() for path in child_paths)
        finally:
            stop_server(proc, drain)


if __name__ == "__main__":
    main(os.path.abspath(sys.argv[1]))
    print("CGI_TESTS_PASS")
