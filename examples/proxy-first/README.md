# Proxy-first demo

```text
client → reverse-proxy:8080 → backend-server:8001
```

Run from the repository root:

```sh
make demo-proxy
```

or start both processes yourself:

```sh
./backend-server --config examples/proxy-first/backend.conf &
./reverse-proxy --config examples/proxy-first/proxy.conf &
```

Then:

```sh
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/cgi-bin/echo?x=1
```

`backend-server` serves static files and CGI and contains no proxy code. The proxy accepts all client traffic and forwards `/` to the backend. It does not serve static files itself.
