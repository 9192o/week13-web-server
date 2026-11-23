#!/usr/bin/env python3

import os
import signal
import socket
import subprocess
import sys
import tempfile
import time


def build_server(src_dir, grace_seconds=2):
    exe = os.path.join(src_dir, "webserver-shutdown-test")
    sources = [
        "app.c", "webserver.c", "epoll_loop.c", "client_session.c", "http.c", "router.c",
        "static_file.c", "static_response.c", "connection.c", "shutdown.c",
    ]
    cmd = [
        "gcc", "-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        f"-DSHUTDOWN_GRACE_SECONDS={grace_seconds}",
        "-I.",
    ] + sources + ["-o", exe]
    subprocess.run(cmd, cwd=src_dir, check=True)
    return exe


def wait_for_line(process, prefix, timeout=10.0):
    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        try:
            chunk = process.stdout.read1(1024)
        except AttributeError:
            chunk = process.stdout.read(1024)
        if not chunk:
            if process.poll() is not None:
                raise RuntimeError(f"process exited early with {process.returncode}")
            time.sleep(0.01)
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if line.startswith(prefix.encode()):
                return line.decode()
    raise TimeoutError(f"did not see {prefix!r} in time")


def connect(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(("127.0.0.1", port))
    return s


def start_server(exe, port, root):
    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"
    proc = subprocess.Popen(
        [exe, "--port", str(port), "--root", root],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    try:
        wait_for_line(proc, "READY")
    except Exception:
        proc.kill()
        proc.wait()
        raise
    return proc


def drain_to_empty(sock, timeout_seconds):
    sock.settimeout(timeout_seconds)
    collected = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            collected += chunk
    except socket.timeout:
        pass
    return collected


def wait_for_exit(proc, timeout_seconds):
    try:
        proc.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        raise AssertionError("server did not exit")
    assert proc.returncode == 0, f"exit code {proc.returncode}"


def test_idle_shutdown(signum, label):
    with tempfile.TemporaryDirectory() as root:
        port = 19080
        proc = start_server(exe, port, root)
        try:
            proc.send_signal(signum)
            wait_for_exit(proc, 5.0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
    print(f"PASS {label}_IDLE_SHUTDOWN")


def test_partial_header_silent_close(signum, label):
    with tempfile.TemporaryDirectory() as root:
        port = 19081
        proc = start_server(exe, port, root)
        try:
            s = connect(port)
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")
            time.sleep(0.2)
            proc.send_signal(signum)
            remainder = drain_to_empty(s, 2.0)
            assert remainder == b"", f"{label}: partial headers got response {remainder!r}"
            s.close()
            wait_for_exit(proc, 5.0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
    print(f"PASS {label}_PARTIAL_HEADER_SILENT_CLOSE")


def test_validated_request_allowed(signum, label):
    with tempfile.TemporaryDirectory() as root:
        port = 19082
        proc = start_server(exe, port, root)
        try:
            s = connect(port)
            body = b"hello-shutdown"
            headers = (
                f"POST /echo HTTP/1.1\r\n"
                f"Host: localhost\r\n"
                f"Content-Length: {len(body)}\r\n"
                f"\r\n"
            ).encode()
            s.sendall(headers)
            time.sleep(0.1)
            proc.send_signal(signum)
            time.sleep(0.1)
            s.sendall(body)
            response = drain_to_empty(s, 5.0)
            assert response.startswith(b"HTTP/1.1 200 OK"), (
                f"{label}: expected 200, got {response[:80]!r}"
            )
            assert response.endswith(body), f"{label}: body not echoed: {response!r}"
            s.close()
            wait_for_exit(proc, 5.0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
    print(f"PASS {label}_VALIDATED_REQUEST_ALLOWED")


def test_validated_request_honors_existing_deadline(signum, label):
    with tempfile.TemporaryDirectory() as root:
        port = 19083
        test_exe = os.path.join(src_dir, "webserver-shutdown-deadline-test")
        cmd = [
            "gcc", "-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-DSHUTDOWN_GRACE_SECONDS=10",
            "-DPHASE1_HEADER_MS=2000",
            "-DPHASE1_IDLE_MS=500",
            "-DPHASE1_TOTAL_MS=2000",
            "-DPHASE1_SEND_MS=2000",
            "-I.",
            "app.c", "webserver.c", "epoll_loop.c", "client_session.c", "http.c", "router.c",
            "static_file.c", "static_response.c", "connection.c", "shutdown.c",
            "-o", test_exe,
        ]
        subprocess.run(cmd, cwd=src_dir, check=True)

        proc = start_server(test_exe, port, root)
        start = time.monotonic()
        try:
            s = connect(port)
            s.sendall(b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n")
            time.sleep(0.1)
            proc.send_signal(signum)
            response = drain_to_empty(s, 5.0)
            elapsed = time.monotonic() - start
            assert elapsed < 5.0, f"{label}: took too long ({elapsed:.1f}s)"
            assert not response.startswith(b"HTTP/1.1 200"), (
                f"{label}: should not have completed: {response[:80]!r}"
            )
            s.close()
            wait_for_exit(proc, 5.0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        print(f"PASS {label}_VALIDATED_HONORS_EXISTING_DEADLINE")


def test_active_response_finishes(signum, label):
    with tempfile.TemporaryDirectory() as root:
        port = 19085
        payload = b"phase-two" * 2097152
        with open(os.path.join(root, "large.png"), "wb") as file:
            file.write(payload)
        proc = start_server(exe, port, root)
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
                s.settimeout(5.0)
                s.connect(("127.0.0.1", port))
                s.sendall(
                    b"GET /large.png HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
                )
                with s.makefile("rb") as wire:
                    assert wire.readline().startswith(b"HTTP/1.1 200 ")
                    while (line := wire.readline()) != b"\r\n":
                        assert line, "EOF in response headers"
                    proc.send_signal(signum)
                    assert wire.read(len(payload)) == payload
                    assert wire.read(1) == b""
            wait_for_exit(proc, 5.0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        print(f"PASS {label}_ACTIVE_RESPONSE_FINISHES")


def test_grace_caps_body_wait(signum, label):
    with tempfile.TemporaryDirectory() as root:
        port = 19086
        proc = start_server(exe, port, root)
        try:
            with connect(port) as s:
                s.sendall(
                    b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 9\r\n"
                    b"Expect: 100-continue\r\n\r\n"
                )
                with s.makefile("rb") as wire:
                    assert wire.readline() == b"HTTP/1.1 100 Continue\r\n"
                    assert wire.readline() == b"\r\n"
                    at_signal = time.monotonic()
                    proc.send_signal(signum)
                    assert wire.read(1) == b""
                    assert 1.0 <= time.monotonic() - at_signal < 5.0
            wait_for_exit(proc, 5.0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        print(f"PASS {label}_GRACE_CAPS_BODY_WAIT")


def test_no_new_accepts_after_signal(signum, label):
    with tempfile.TemporaryDirectory() as root:
        port = 19084
        proc = start_server(exe, port, root)
        try:
            proc.send_signal(signum)
            time.sleep(0.2)
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(1.0)
            try:
                s.connect(("127.0.0.1", port))
            except (socket.timeout, ConnectionRefusedError):
                s.close()
            else:
                data = drain_to_empty(s, 2.0)
                s.close()
                assert data == b"", f"{label}: late connection got data {data!r}"

            wait_for_exit(proc, 5.0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
    print(f"PASS {label}_NO_NEW_ACCEPTS")


def main():
    global exe, src_dir
    src_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = build_server(src_dir, grace_seconds=2)

    for signum, label in [(signal.SIGINT, "SIGINT"), (signal.SIGTERM, "SIGTERM")]:
        test_idle_shutdown(signum, label)
        test_partial_header_silent_close(signum, label)
        test_validated_request_allowed(signum, label)
        test_validated_request_honors_existing_deadline(signum, label)
        test_active_response_finishes(signum, label)
        test_grace_caps_body_wait(signum, label)
        test_no_new_accepts_after_signal(signum, label)

    print("SHUTDOWN_TESTS_PASS")


if __name__ == "__main__":
    main()
