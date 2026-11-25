# Proxy-first demo

```text
client → reverse-proxy:8080 → server --backend:8001
```

Run from the repository root:

```sh
make demo-proxy
```

or start both processes yourself:

```sh
./server --backend --config examples/proxy-first/backend.conf &
./reverse-proxy --config examples/proxy-first/proxy.conf &
```

Then:

```sh
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/cgi-bin/echo?x=1
```

The backend serves static files and CGI. The proxy accepts all client traffic and forwards `/` to the backend. It does not serve static files itself.
