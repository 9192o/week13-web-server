import hashlib
import http.server
import json
import socket
import sys
import time

identity = sys.argv[1].encode()
port = int(sys.argv[2])

class Backend(http.server.ThreadingHTTPServer):
    request_queue_size = 128

    def get_request(self):
        conn, address = super().get_request()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return conn, address

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def serve(self):
        declared = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(declared)
        if "--quiet" not in sys.argv:
            print(json.dumps({"backend": identity.decode(), "method": self.command,
                              "path": self.path, "declared": declared, "received": len(body),
                              "sha256": hashlib.sha256(body).hexdigest()}), flush=True)
        if len(body) != declared:
            self.close_connection = True
            return
        endpoint = self.path.rsplit("/", 1)[-1]
        if endpoint == "silent":
            time.sleep(14)
            self.close_connection = True
            return
        payload = {
            "small": lambda: b"x" * 1024,
            "big": lambda: b"\0\xde\xff\x01" * (2 * 1024 * 1024),
        }.get(endpoint, lambda: b"|".join((identity, self.command.encode(), self.path.encode(), body)))()
        self.send_response(404 if endpoint == "missing" else 200)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        if endpoint == "truncated":
            payload = payload[:2]
            self.close_connection = True
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass

    do_GET = do_POST = serve

with Backend(("127.0.0.1", port), Handler) as service:
    service.serve_forever()
