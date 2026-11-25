#!/usr/bin/env python3
"""팀 공통 테스트 4절(F02–F07, E01–E07) 자동 실행. 사용: python3 func.py <eun|woong|ldc>"""
import hashlib
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

QA = Path("/workspace/qa")
ONE = QA / "one"
CAND = sys.argv[1]
LOG = QA / "logs" / CAND
LOG.mkdir(parents=True, exist_ok=True)
DIRECT, PROXY, ALT, PA, PB = 20801, 20802, 20807, 20803, 20804

CFG = {
    "eun": dict(dir="/workspace/eun",
                direct=["./server", "-p", "20801", "-r", f"{QA}/one"],
                alternate=["./server", "-p", "20807", "-r", f"{QA}/two"],
                proxy=["./server", "-p", "20802", "-r", f"{QA}/one",
                       "-R", "/a/=127.0.0.1:20803", "-R", "/b/=127.0.0.1:20804"],
                uri=("uri", 8192), head=16384, body=1048576, head_to=5, be_to=5),
    "woong": dict(dir="/workspace/woong",
                  direct=["./webserver", "--port", "20801", "--root", f"{QA}/one"],
                  alternate=["./webserver", "--port", "20807", "--root", f"{QA}/two"],
                  proxy=["./reverse-proxy", "--port", "20802",
                         "--route", "/a/=127.0.0.1:20803", "--route", "/b/=127.0.0.1:20804"],
                  uri=("line", 8192), head=32768, body=1048576, head_to=10, be_to=5),
    "ldc": dict(dir="/workspace/ldc",
                direct=["./server", "20801", f"{QA}/one"],
                alternate=["./server", "20807", f"{QA}/two"],
                proxy=["./server", "--config", f"{QA}/proxy.conf"],
                uri=("uri", 4095), head=16384, body=8388608, head_to=20, be_to=10),
}
C = CFG[CAND]
MEM = len(sys.argv) > 2 and sys.argv[2] == "mem"
if MEM:  # M01: 일반 빌드를 Valgrind Memcheck 아래에서 기동
    VG = ["valgrind", "--leak-check=full", "--show-leak-kinds=all",
          "--errors-for-leak-kinds=definite,indirect,possible", "--error-exitcode=77"]
    for _n in ("direct", "proxy"):
        C[_n] = VG + [f"--log-file={LOG}/mem-{_n}-%p.log"] + C[_n]
(QA / "proxy.conf").write_text(
    "listen 20802\ndocroot %s/one\n[route /a/]\ntype proxy\nbackend 127.0.0.1:20803\n"
    "[route /b/]\ntype proxy\nbackend 127.0.0.1:20804\n[route /]\ntype static\n" % QA)

OUT = open(LOG / "func.jsonl", "w")
COUNT = {}


def rec(cid, sub, verdict, **obs):
    COUNT[verdict] = COUNT.get(verdict, 0) + 1
    row = {"id": cid, "sub": sub, "verdict": verdict, **obs}
    OUT.write(json.dumps(row, ensure_ascii=False, default=str) + "\n")
    OUT.flush()
    print(f"{verdict} {cid} {sub}: " +
          json.dumps(obs, ensure_ascii=False, default=str)[:260], flush=True)


# ------------------------------------------------------------------ 프로세스

PROCS = {}


def start(name, argv, cwd=None):
    f = open(LOG / f"{name}.log", "ab")
    PROCS[name] = subprocess.Popen(argv, cwd=cwd or C["dir"], stdout=f,
                                   stderr=subprocess.STDOUT)
    return PROCS[name]


def stop(name, sig=signal.SIGTERM, t=20):
    p = PROCS.pop(name, None)
    if p is None or p.poll() is not None:
        return p.returncode if p else None
    p.send_signal(sig)
    try:
        return p.wait(t)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        return f"timeout({t}s)->SIGKILL"


def wait_listen(port, t=10):
    end = time.monotonic() + t
    while time.monotonic() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def start_backend(ident, port):
    start(f"backend-{ident.lower()}", ["python3", str(QA / "backend.py"), ident, str(port)], cwd=str(QA))
    return wait_listen(port)


def start_server(name):
    start(name, C[name])
    return wait_listen({"direct": DIRECT, "proxy": PROXY, "alternate": ALT}[name])


# ------------------------------------------------------------------ HTTP 클라이언트

class Conn:
    def __init__(self, port, timeout=5.0):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buf = b""

    def send(self, b):
        self.s.sendall(b)

    def fill(self, n=65536):
        try:
            d = self.s.recv(n)
        except ConnectionResetError:
            d = b""
        if not d:
            raise EOFError
        self.buf += d

    def head(self):
        while True:
            while b"\r\n\r\n" not in self.buf:
                try:
                    self.fill()
                except EOFError:
                    return None
            head, self.buf = self.buf.split(b"\r\n\r\n", 1)
            lines = head.decode("latin1").split("\r\n")
            parts = lines[0].split(" ", 2)
            status = int(parts[1])
            hdr = {}
            for line in lines[1:]:
                if ":" in line:
                    k, v = line.split(":", 1)
                    hdr[k.strip().lower()] = v.strip()
            if 100 <= status < 200:
                continue
            return status, hdr

    def response(self):
        h = self.head()
        if h is None:
            return {"status": None, "eof_before_head": True, "raw": self.buf[:120]}
        status, hdr = h
        body, complete = b"", True
        if "content-length" in hdr:
            n = int(hdr["content-length"])
            while len(self.buf) < n:
                try:
                    self.fill()
                except EOFError:
                    complete = False
                    break
            body, self.buf = self.buf[:n], self.buf[n:]
        elif "chunked" in hdr.get("transfer-encoding", "").lower():
            body, complete = self.chunked()
        else:
            while True:
                try:
                    self.fill()
                except EOFError:
                    break
            body, self.buf = self.buf, b""
        return {"status": status, "headers": hdr, "body": body, "complete": complete}

    def chunked(self):
        out = b""
        while True:
            while b"\r\n" not in self.buf:
                try:
                    self.fill()
                except EOFError:
                    return out, False
            line, self.buf = self.buf.split(b"\r\n", 1)
            n = int(line.split(b";")[0], 16)
            while len(self.buf) < n + 2:
                try:
                    self.fill()
                except EOFError:
                    return out + self.buf, False
            out += self.buf[:n]
            self.buf = self.buf[n + 2:]
            if n == 0:
                return out, True

    def eof_within(self, t):
        if self.buf:
            return False
        self.s.settimeout(t)
        try:
            return self.s.recv(1) == b""
        except socket.timeout:
            return None
        except ConnectionResetError:
            return True

    def close(self):
        self.s.close()


def exch(port, data, timeout=5.0, shut=False, check_close=False):
    t0 = time.monotonic()
    try:
        c = Conn(port, timeout)
        for i, p in enumerate(data if isinstance(data, (list, tuple)) else [data]):
            if i:
                time.sleep(0.02)
            c.send(p)
        if shut:
            c.s.shutdown(socket.SHUT_WR)
        r = c.response()
        if check_close and r.get("status"):
            r["eof_after"] = c.eof_within(1.0)
        c.close()
    except socket.timeout:
        r = {"status": None, "error": "client-timeout"}
    except OSError as e:
        r = {"status": None, "error": f"{type(e).__name__}: {e}"}
    r["elapsed"] = round(time.monotonic() - t0, 3)
    return r


def get_raw(path, close=True):
    return (f"GET {path} HTTP/1.1\r\nHost: localhost\r\n"
            + ("Connection: close\r\n" if close else "") + "\r\n").encode()


def post_raw(path, body):
    return (f"POST {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
            f"Content-Length: {len(body)}\r\n\r\n").encode() + body


def get(port, path, **kw):
    return exch(port, get_raw(path), **kw)


def brief(r):
    b = r.get("body")
    out = {k: r[k] for k in ("status", "elapsed", "error", "complete", "eof_after",
                              "eof_before_head") if k in r}
    if b is not None:
        out["body_len"] = len(b)
        out["body_head"] = b[:40]
    h = r.get("headers") or {}
    for k in ("content-type", "content-length", "connection"):
        if k in h:
            out[k] = h[k]
    return out


def recover_direct():
    r = get(DIRECT, "/file.html")
    return r.get("status") == 200 and r.get("body") == (ONE / "file.html").read_bytes()


def recover_proxy():
    r = get(PROXY, "/a/ok")
    return r.get("status") == 200 and r.get("body") == b"A|GET|/a/ok|"


def recover(port):
    return recover_direct() if port == DIRECT else recover_proxy()


def is_err(r):
    """요청이 성공으로 처리되지 않았는가: 4xx/5xx 응답 또는 응답 없이 종료."""
    s = r.get("status")
    return s is None and r.get("error") != "client-timeout" or (s is not None and s >= 400)


def sha(b):
    return hashlib.sha256(b).hexdigest()


BIG_SHA = sha((ONE / "big.png").read_bytes())


def backend_log(ident):
    rows = []
    for line in (LOG / f"backend-{ident.lower()}.log").read_text().splitlines():
        try:
            rows.append(json.loads(line))
        except ValueError:
            pass
    return rows


# ------------------------------------------------------------------ 케이스

def f02_f01():
    mime = {"html": ("text/html",), "css": ("text/css",),
            "js": ("text/javascript", "application/javascript"),
            "png": ("image/png",), "jpg": ("image/jpeg",)}
    for ext in ("html", "css", "js", "png", "jpg"):
        exp = (ONE / f"file.{ext}").read_bytes()
        r = get(DIRECT, f"/file.{ext}")
        h = r.get("headers") or {}
        base = h.get("content-type", "").split(";")[0].strip().lower()
        cl = h.get("content-length")
        ok = (r.get("status") == 200 and r.get("body") == exp and base in mime[ext]
              and (cl is None or int(cl) == len(exp)))
        rec("F02", f"/file.{ext}", "통과" if ok else "실패", **brief(r), bytes_equal=r.get("body") == exp)
    if MEM:
        return
    ok_start = start_server("alternate")
    r = get(ALT, "/file.html")
    ok = ok_start and r.get("status") == 200 and r.get("body") == b"other root"
    code = stop("alternate")
    rec("F01", "포트 20807·루트 two 전환", "통과" if ok else "실패", **brief(r), alt_exit=code)


def f03():
    for fn in ("post-text.bin", "post-binary.bin"):
        data = (QA / fn).read_bytes()
        head = (f"POST /a/echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                f"Content-Length: {len(data)}\r\n\r\n").encode()
        for label, pieces in (("한 번에", (head + data,)),
                              ("나눠서", (head[:-1], head[-1:] + data[:1], data[1:]))):
            r = exch(PROXY, pieces)
            ok = r.get("status") == 200 and r.get("body") == b"A|POST|/a/echo|" + data
            rec("F03", f"{fn} {label}", "통과" if ok else "실패", **brief(r))


def f04():
    r = get(DIRECT, "/missing.html")
    rec("F04", "/missing.html → 404", "통과" if r.get("status") == 404 else "실패", **brief(r))
    r = get(DIRECT, "/locked.html")
    rec("F04", "/locked.html → 403 (uid 1000)", "통과" if r.get("status") == 403 else "실패", **brief(r))
    rec("F04", "통제된 내부 I/O 실패 → 500", "미실행",
        note="컨테이너 안에서 통제된 I/O 실패를 만들 수단 없음, U05 매핑은 계획 문서 필요")


def f06():
    data = (QA / "post-binary.bin").read_bytes()
    for path, ident, status in (("/a/ok", "A", 200), ("/b/missing", "B", 404)):
        r = get(PROXY, path)
        ok = r.get("status") == status and r.get("body") == f"{ident}|GET|{path}|".encode()
        rec("F06", f"GET {path} → {ident}·{status}", "통과" if ok else "실패", **brief(r))
        r = exch(PROXY, post_raw(path, data))
        ok = r.get("status") == status and r.get("body") == f"{ident}|POST|{path}|".encode() + data
        time.sleep(0.2)
        seen = [x for x in backend_log(ident) if x["path"] == path and x["method"] == "POST"]
        ok_log = bool(seen) and seen[-1]["received"] == len(data) and seen[-1]["sha256"] == sha(data)
        rec("F06", f"POST {path} 바이너리 → {ident}·{status}",
            "통과" if ok and ok_log else "실패", **brief(r), backend_entry=seen[-1] if seen else None)


def f05():
    png = (ONE / "file.png").read_bytes()

    def task(i):
        k = i % 3
        if k == 0:
            r = get(DIRECT, "/file.png")
            return r.get("status") == 200 and r.get("body") == png
        if k == 1:
            ident = f"id-{i:02d}".encode()
            r = exch(PROXY, post_raw("/a/echo", ident))
            return r.get("status") == 200 and r.get("body") == b"A|POST|/a/echo|" + ident
        r = get(PROXY, "/b/ok")
        return r.get("status") == 200 and r.get("body") == b"B|GET|/b/ok|"

    def batch():
        res = [None] * 16
        ths = [threading.Thread(target=lambda i=i: res.__setitem__(i, task(i))) for i in range(16)]
        t0 = time.monotonic()
        for t in ths:
            t.start()
        for t in ths:
            t.join()
        return res, round(time.monotonic() - t0, 3)

    res, el = batch()
    rec("F05", "정상 16연결 묶음", "통과" if all(res) else "실패", ok=sum(map(bool, res)), elapsed=el)
    slow = Conn(DIRECT, C["head_to"] + 5)
    slow.send(b"GET /file.html HTTP/1.1\r\nHost: localhost\r\n")
    res, el = batch()
    slow.send(b"Connection: close\r\n\r\n")
    try:
        sr = slow.response()
    except socket.timeout:
        sr = {"status": None, "error": "client-timeout"}
    slow.close()
    rec("F05", "미완성 헤드 연결을 열어 둔 채 같은 16연결", "통과" if all(res) else "실패",
        ok=sum(map(bool, res)), elapsed=el, slow_conn_after=brief(sr))


def f07():
    for port, path in ((DIRECT, "/file.html"), (PROXY, "/a/ok")):
        try:
            c = Conn(port, 5)
            c.send(get_raw(path, close=False))
            r1 = c.response()
            conn_hdr = (r1.get("headers") or {}).get("connection", "").lower()
            if "close" in conn_hdr:
                eof = c.eof_within(2)
                ok = r1.get("status") == 200 and eof is True
                rec("F07", f"{port} 종료 모드", "통과" if ok else "실패", **brief(r1), eof_after=eof)
            else:
                try:
                    c.send(get_raw(path, close=False))
                    r2 = c.response()
                except OSError as e:
                    r2 = {"status": None, "error": str(e)}
                ok = r1.get("status") == 200 and r2.get("status") == 200 and r2.get("body") == r1.get("body")
                rec("F07", f"{port} 재사용 모드 (응답 2개)", "통과" if ok else "실패",
                    first=brief(r1), second=brief(r2))
            c.close()
        except OSError as e:
            rec("F07", f"{port}", "실패", error=str(e))


def e01():
    cases = (("문법이 틀린 요청 줄", b"GARBAGE\r\n\r\n"),
             ("지원하지 않는 메서드", b"BREW /file.html HTTP/1.1\r\nHost: localhost\r\n\r\n"),
             ("지원하지 않는 버전", b"GET /file.html HTTP/2.0\r\nHost: localhost\r\n\r\n"),
             ("숫자가 아닌 Content-Length",
              b"POST /a/echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: abc\r\n\r\n"))
    for port in (DIRECT, PROXY):
        for label, raw in cases:
            r = exch(port, raw, check_close=True)
            ok_rec = recover(port)
            ok = is_err(r) and ok_rec
            rec("E01", f"{port} {label}", "통과" if ok else "실패", **brief(r), recovery=ok_rec)


def e02():
    lim = C["head_to"] + 2
    for port in (DIRECT, PROXY):
        path = "/file.html" if port == DIRECT else "/a/ok"
        r = exch(port, f"GET {path} HTTP/1.1\r\nHost: loc".encode(), shut=True, timeout=lim)
        ok_rec = recover(port)
        rec("E02", f"{port} 헤드 중간에 쓰기 닫기", "통과" if is_err(r) and ok_rec else "실패",
            **brief(r), recovery=ok_rec)
        r = exch(port, f"GET {path} HTTP/1.1\r\nHost: loc".encode(), timeout=lim)
        ok_rec = recover(port)
        ok = r.get("error") != "client-timeout" and is_err(r) and ok_rec
        rec("E02", f"{port} 헤드 중간에 열어 둔 채 대기 (한도 {lim}s)", "통과" if ok else "실패",
            **brief(r), recovery=ok_rec)
    raw = b"POST /a/e02-partial HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\nabc"
    r = exch(PROXY, raw, shut=True, timeout=lim)
    time.sleep(0.5)
    seen = [x for x in backend_log("A") if x["path"] == "/a/e02-partial"]
    ok_rec = recover(PROXY)
    ok = is_err(r) and ok_rec and all(x["received"] == x["declared"] or x["received"] < x["declared"] for x in seen)
    rec("E02", "프록시 POST 본문 3/10 뒤 쓰기 닫기", "통과" if ok and r.get("status") != 200 else "실패",
        **brief(r), backend_entries=seen, recovery=ok_rec)


def boundary(port, raw, timeout=15.0):
    """헤드를 보내고 0.5초 안에 응답이 오면 '선언 단계 거부', 아니면 나머지를 보냄."""
    t0 = time.monotonic()
    try:
        c = Conn(port, timeout)
        if isinstance(raw, tuple):
            head, body = raw
            c.send(head)
            c.s.settimeout(0.5)
            try:
                c.fill()
                early = True
            except socket.timeout:
                early = False
            except EOFError:
                early = True
            c.s.settimeout(timeout)
            if not early:
                try:
                    c.send(body)
                except OSError:
                    pass
        else:
            early = None
            c.send(raw)
        r = c.response()
        c.close()
    except socket.timeout:
        r, early = {"status": None, "error": "client-timeout"}, None
    except OSError as e:
        r, early = {"status": None, "error": f"{type(e).__name__}: {e}"}, None
    r["elapsed"] = round(time.monotonic() - t0, 3)
    r["declared_stage_reject"] = early
    return r


def e03():
    kind, L = C["uri"]
    fixed = (ONE / "file.html").read_bytes()
    for d in (-1, 0, 1):
        n = L + d
        if kind == "uri":
            target = "/file.html?" + "q" * (n - len("/file.html?"))
            assert len(target) == n
            raw = f"GET {target} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode()
            label = f"URI {n} B"
        else:
            pre, post = "GET /file.html?", " HTTP/1.1\r\n"
            line = pre + "q" * (n - len(pre) - len(post)) + post
            assert len(line) == n
            raw = (line + "Host: localhost\r\nConnection: close\r\n\r\n").encode()
            label = f"요청 줄 {n} B (CRLF 포함)"
        r = boundary(DIRECT, raw)
        want_ok = d <= 0
        ok = (r.get("status") == 200 and r.get("body") == fixed) if want_ok else is_err(r)
        ok_rec = recover_direct()
        rec("E03", f"{label} ({'+' if d > 0 else ''}{d})", "통과" if ok and ok_rec else "실패",
            **brief(r), recovery=ok_rec)
    H = C["head"]
    for d in (-1, 0, 1):
        n = H + d
        base = "GET /file.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
        pad_total = n - len(base) - 2
        lines, i = [], 0
        while pad_total > 0:
            name = f"X-Pad-{i:03d}: "
            size = min(1000, pad_total)
            if pad_total - size and pad_total - size < len(name) + 3:
                size = pad_total - (len(name) + 3)
            val = "a" * (size - len(name) - 2)
            lines.append(name + val + "\r\n")
            pad_total -= size
            i += 1
        raw = (base + "".join(lines) + "\r\n").encode()
        assert len(raw) == n, (len(raw), n)
        r = boundary(DIRECT, raw)
        want_ok = d <= 0
        ok = (r.get("status") == 200 and r.get("body") == fixed) if want_ok else is_err(r)
        ok_rec = recover_direct()
        rec("E03", f"전체 헤드 {n} B ({'+' if d > 0 else ''}{d}, 헤더 {i + 2}개)",
            "통과" if ok and ok_rec else "실패", **brief(r), recovery=ok_rec)
    B = C["body"]
    for d in (-1, 0, 1):
        n = B + d
        body = b"b" * n
        head = (f"POST /a/echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                f"Content-Length: {n}\r\n\r\n").encode()
        r = boundary(PROXY, (head, body), timeout=30)
        want_ok = d <= 0
        ok = (r.get("status") == 200 and r.get("body") == b"A|POST|/a/echo|" + body) if want_ok else is_err(r)
        ok_rec = recover_proxy()
        rr = brief(r)
        rr["body_head"] = rr.get("body_head", b"")[:20]
        rec("E03", f"본문 {n} B ({'+' if d > 0 else ''}{d})", "통과" if ok and ok_rec else "실패",
            **rr, declared_stage_reject=r.get("declared_stage_reject"), recovery=ok_rec)


def slow_read(port, path, chunk=16384, pause=0.005, stop_after=None):
    c = Conn(port, 30)
    c.send(get_raw(path))
    t0 = time.monotonic()
    while b"\r\n\r\n" not in c.buf:
        c.fill(chunk)
    head, rest = c.buf.split(b"\r\n\r\n", 1)
    status = int(head.split(b" ")[1])
    cl = None
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            cl = int(line.split(b":")[1])
    body = rest
    c.buf = b""
    while cl is None or len(body) < cl:
        if stop_after and len(body) >= stop_after:
            c.close()
            return status, cl, len(body), None, round(time.monotonic() - t0, 3)
        try:
            c.fill(chunk)
        except EOFError:
            break
        body += c.buf
        c.buf = b""
        time.sleep(pause)
    c.close()
    return status, cl, len(body), sha(body), round(time.monotonic() - t0, 3)


def e04():
    for port, path in ((DIRECT, "/big.png"), (PROXY, "/a/big")):
        try:
            st, cl, n, h, el = slow_read(port, path)
            ok = st == 200 and h == BIG_SHA
            rec("E04", f"{port} {path} 천천히 읽기", "통과" if ok else "실패",
                status=st, content_length=cl, received=n, sha_ok=h == BIG_SHA, elapsed=el)
        except (OSError, EOFError) as e:
            rec("E04", f"{port} {path} 천천히 읽기", "실패", error=f"{type(e).__name__}: {e}")
        try:
            st, cl, n, _, el = slow_read(port, path, stop_after=100000)
        except (OSError, EOFError) as e:
            st, n = None, str(e)
        ok_rec = recover(port)
        rec("E04", f"{port} {path} 앞부분만 읽고 닫기 → 새 GET", "통과" if ok_rec else "실패",
            status=st, received=n, recovery=ok_rec)


def e07():
    secret = b"E07-OUTSIDE-ROOT-SYNTHETIC-7f3a"
    (QA / "outside-secret.txt").write_bytes(secret)
    link = ONE / "escape.html"
    if not link.is_symlink():
        link.symlink_to("../outside-secret.txt")
    paths = ("/../outside-secret.txt", "/%2e%2e/outside-secret.txt", "/%2E%2E%2Foutside-secret.txt",
             "/..%2Foutside-secret.txt", "/./../outside-secret.txt", "//../outside-secret.txt",
             "/escape.html")
    for p in paths:
        r = exch(DIRECT, get_raw(p))
        leaked = secret in (r.get("body") or b"")
        ok_rec = recover_direct()
        rec("E07", p, "통과" if not leaked and ok_rec else "실패", **brief(r), leaked=leaked, recovery=ok_rec)
    link.unlink()


def e06():
    t0 = time.monotonic()
    code = stop("backend-b")
    r = get(PROXY, "/b/ok", timeout=C["be_to"] + 2)
    ok = r.get("status") is not None and r["status"] >= 500 or is_err(r)
    rec("E06", "백엔드 B 종료 뒤 /b/ok", "통과" if ok and r.get("error") != "client-timeout" else "실패",
        **brief(r), b_exit=code)
    start_backend("B", PB)
    t1 = time.monotonic()
    got = None
    while time.monotonic() - t1 < 15:
        rr = get(PROXY, "/b/ok", timeout=3)
        if rr.get("status") == 200 and rr.get("body") == b"B|GET|/b/ok|":
            got = round(time.monotonic() - t1, 3)
            break
        time.sleep(0.1)
    rec("E06", "B 재기동 뒤 /b/ok 복구", "통과" if got is not None else "실패", recovery_seconds=got)
    lim = C["be_to"] + 2
    r = get(PROXY, "/a/silent", timeout=lim)
    ok = r.get("error") != "client-timeout" and is_err(r)
    rec("E06", f"/a/silent (백엔드 무응답, 한도 {lim}s)", "통과" if ok else "실패", **brief(r),
        recovery=recover_proxy())
    r = exch(PROXY, get_raw("/a/truncated"), timeout=lim)
    full = b"A|GET|/a/truncated|"
    complete_success = r.get("status") == 200 and r.get("complete") and r.get("body") == full
    lied = r.get("status") == 200 and r.get("complete") and r.get("body") != full
    ok = not complete_success and not lied
    rec("E06", "/a/truncated (백엔드가 본문 2바이트 후 끊음)", "통과" if ok else "실패", **brief(r),
        declared_by_backend=len(full), recovery=recover_proxy())
    del t0


def e05():
    for name, port, big, small, exp_small in (
            ("direct", DIRECT, "/big.png", "/small.html", b"x" * 1024),
            ("proxy", PROXY, "/a/big", "/a/small", b"x" * 1024)):
        p2 = start(f"{name}-second", C[name])
        try:
            code2 = p2.wait(5)
        except subprocess.TimeoutExpired:
            p2.kill()
            p2.wait()
            code2 = "5초 안에 종료 안 함"
        alive = PROCS[name].poll() is None
        rec("E05", f"{name} 같은 설정 두 번째 프로세스", "통과" if code2 not in (0, "5초 안에 종료 안 함") and alive else "실패",
            second_exit=code2, first_alive=alive, recovery=recover(port))
        for sig in (signal.SIGINT, signal.SIGTERM):
            stop(name)
            start_server(name)
            obs = {}
            try:
                c = Conn(port, 30)
                c.send(get_raw(big))
                while len(c.buf) < 262144:
                    c.fill()
                PROCS[name].send_signal(sig)
                time.sleep(0.3)
                nr = get(port, small, timeout=3)
                obs["new_conn_during"] = brief(nr)
                head, body = c.buf.split(b"\r\n\r\n", 1)
                c.buf = b""
                c.s.settimeout(15)
                while True:
                    try:
                        c.fill()
                    except (EOFError, socket.timeout):
                        break
                    body += c.buf
                    c.buf = b""
                c.close()
                obs["inflight_received"] = len(body)
                obs["inflight_sha_ok"] = sha(body) == BIG_SHA
            except (OSError, EOFError) as e:
                obs["error"] = f"{type(e).__name__}: {e}"
            p = PROCS.pop(name)
            try:
                obs["exit"] = p.wait(20)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
                obs["exit"] = "20초 안에 종료 안 함 → SIGKILL"
            ok_started = start_server(name)
            rr = get(port, small)
            obs["restart_get"] = rr.get("status") == 200 and rr.get("body") == exp_small
            ok = (obs.get("inflight_sha_ok") and isinstance(obs.get("exit"), int)
                  and obs["restart_get"] and ok_started)
            rec("E05", f"{name} 큰 응답 중 {sig.name}", "통과" if ok else "실패", **obs)


def main():
    global OUT
    if MEM:
        OUT = open(LOG / "mem-func.jsonl", "w")
    assert start_backend("A", PA) and start_backend("B", PB), "backend start failed"
    assert start_server("direct"), "direct start failed"
    assert start_server("proxy"), "proxy start failed"
    if MEM:
        time.sleep(3)  # valgrind 기동 여유
        cases = (f02_f01, f03, f04, f06, e02, e04, e06)
    else:
        cases = (f02_f01, f03, f04, f06, f05, f07, e01, e02, e03, e04, e07, e06, e05)
    try:
        for fn in cases:
            try:
                fn()
            except Exception as e:  # 하네스 오류는 실패가 아니라 따로 기록
                rec(fn.__name__.upper(), "하네스 오류", "미실행", error=f"{type(e).__name__}: {e}")
    finally:
        for n in list(PROCS):
            pid = PROCS[n].pid
            code = stop(n, t=120 if MEM else 20)
            print(f"stop {n} pid={pid}: exit={code}", flush=True)
            if MEM and n in ("direct", "proxy"):
                rec("M01", f"{n} valgrind 종료 코드", "통과" if code == 0 else "실패", pid=pid, exit=code)
        print("SUMMARY", CAND, COUNT, flush=True)


main()
