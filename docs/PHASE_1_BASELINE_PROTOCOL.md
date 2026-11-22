# Phase 1 Performance Baseline Protocol

Version: 2026-11-22

Scope: P1-30 / T-30 / D-08 performance baseline for the Phase 1 web server.
Status: benchmark profile decided; host/image/mount/resource-limit fields are recorded in each run output rather than fixed here.

## 1. Server under test

The harness uses the CLI contract documented in `PHASE_1_REQUIREMENTS.md` (P1-03) and `PHASE_1_FINISH_PLAN.md` (D-07):

```bash
./webserver --port <1..65535> --root <directory> [extra server args...]
```

- The server binds IPv4 loopback (`127.0.0.1`) on the supplied port.
- After `bind()` succeeds it prints exactly one line to stdout in the form `READY <port>\n`, where `<port>` is the TCP port the server bound to.  The harness parses this line and uses the reported port for all requests.
- Static files are served from `--root`.  `POST /echo` returns the request body bytes unchanged.
- Each response uses `Connection: close`; the harness opens a fresh TCP connection for every request.

## 2. Workloads

All payloads are deterministic so repeated runs can be compared.  The payload pattern is `bytes(range(256))` repeated as needed.

| Category | Path | Encoding | Body / file size |
| --- | --- | --- | --- |
| static GET | `/webserver_bench_static_1k.png` | none | 1 KiB |
| static GET | `/webserver_bench_static_64k.png` | none | 64 KiB |
| static GET | `/webserver_bench_static_2m.png` | none | 2 MiB (> 1 MiB) |
| echo POST | `/echo` | `Content-Length` | 0 B |
| echo POST | `/echo` | `Content-Length` | 1 KiB |
| echo POST | `/echo` | `Content-Length` | 64 KiB |
| echo POST | `/echo` | `Content-Length` | 1 MiB |
| echo POST | `/echo` | `Transfer-Encoding: chunked` | 0 B |
| echo POST | `/echo` | `Transfer-Encoding: chunked` | 1 KiB |
| echo POST | `/echo` | `Transfer-Encoding: chunked` | 64 KiB |
| echo POST | `/echo` | `Transfer-Encoding: chunked` | 1 MiB |

The harness creates the three static fixture files in `--root` before a full run starts.  Fixture names are prefixed with `webserver_bench_` and use the `.png` extension to align with the public MIME allowlist.  If `--root` is omitted the harness uses a temporary directory.  The harness never writes files outside these fixture names.  For every request the harness verifies status `200` and byte-exact body equality.

## 3. Connection model

- Sequential: one request at a time.
- Fresh connection: each request uses a new `socket.create_connection((127.0.0.1, port))` and reads the full response to EOF.
- No keep-alive, pipelining, or concurrent clients.  This profile isolates per-request cost in the current blocking sequential implementation.

## 4. Measurement procedure

Fixed counts (not configurable from the command line):

```text
warmup_requests  = 1
sample_requests  = 30
```

Configurable counts:

```text
--repeat  default 3   (one measurement run per workload)
```

Per workload run:

1. Execute `warmup_requests` unmeasured requests.  A warmup failure aborts the workload.
2. Execute `sample_requests` measured requests back-to-back.
3. Record per-request latency from immediately before connect to after the full body is received.
4. Count errors (unexpected status, body mismatch, timeout, socket exception).

Timing boundaries:

- `throughput_rps` = `successful_sample_requests / sample_elapsed_seconds`.
- `sample_elapsed_seconds` is the wall time between the start of the first sample request and the end of the last sample request.
- Latency percentiles (`p50`, `p95`, `p99`) are computed only from successful sample latencies, in milliseconds.

Resource monitoring:

- Read `/proc/<server_pid>/statm` for RSS pages and `/proc/<server_pid>/fd` for open FD count.
- A background sampler thread records RSS/FD every 50 ms while request traffic is running (between the first warmup and the end of the last sample).  This captures usage during actual traffic rather than only post-run snapshots.
- Snapshots:
  1. `before`: after the server is ready and before the traffic sampler starts.
  2. `peak`: the maximum RSS/FD observed by the traffic sampler.
  3. `after`: just before the server is stopped.
- Causality note: the sampler is concurrent with request handling; it observes resource state rather than causing it.  It does not participate in request I/O and is kept short to avoid perturbing latency measurements.
- On non-Linux hosts these fields are reported as `null` rather than failing the benchmark.

## 5. Command-line interface of the harness

```text
usage: benchmark_phase1.py <executable> [options]
```

Positional:

- `executable`: path to the server binary (or any executable that honours the CLI contract above).

Options:

- `--port PORT`: server port.  If omitted the harness picks a free loopback port.
- `--root DIR`: optional parent for the temporary document root; the harness removes its own fixture directory after the run and does not modify existing fixture names in `DIR`.
- `--repeat N`: number of measurement runs per workload (default 3).
- `--log-file PATH`: where to redirect the server's stdout+stderr.  Default is a temporary file.  `/dev/null` is allowed.  The path is recorded in the JSON output.
- `--server-arg ARG`: repeatable; each `ARG` is appended to the server command line after `--root`.
- `--output PATH`: write JSON to `PATH` instead of stdout.
- `--echo-only`: run only the echo workloads.  Use this while general static file serving is not yet ready; the resulting JSON is explicitly marked as a partial echo baseline, not a full Phase 1 baseline.
- `--help`: show usage.

The harness validates `--port` to be in `1..65535` before starting the server.

## 6. Output schema

The harness prints a single JSON object.  Example (values are illustrative):

```json
{
  "metadata": {
    "timestamp_utc": "2026-11-22T12:00:00",
    "hostname": "host",
    "platform": "Linux-6.18-x86_64",
    "python_version": "3.13.5",
    "page_size": 4096,
    "server_executable": "/workspace/webserver",
    "server_args": ["--port", "54321", "--root", "/tmp/..."],
    "server_log_file": "/tmp/...log",
    "host": "127.0.0.1",
    "port": 54321,
    "root": "/tmp/...",
    "repeat": 3,
    "warmup_requests": 1,
    "sample_requests": 30,
    "echo_only": false
  },
  "environment": {
    "rss_bytes_before": 1234567,
    "fd_count_before": 4,
    "rss_bytes_peak": 2345678,
    "fd_count_peak": 7,
    "rss_bytes_after": 1234567,
    "fd_count_after": 4
  },
  "summary": {
    "total_errors": 0,
    "total_successful_requests": 1080
  },
  "workloads": [
    {
      "name": "static_1k_get",
      "category": "static",
      "encoding": null,
      "path": "/static_1k.bin",
      "request_size": 1024,
      "repeat": 3,
      "warmup_requests": 1,
      "sample_requests": 30,
      "results": [
        {
          "run": 1,
          "successful": 30,
          "errors": 0,
          "throughput_rps": 1234.5,
          "elapsed_ms": 24.3,
          "p50_ms": 0.78,
          "p95_ms": 0.92,
          "p99_ms": 1.10
        }
      ]
    }
  ]
}
```

`encoding` is `null` for static, `"cl"` for `Content-Length` echo, and `"chunked"` for chunked echo.

## 7. Partial baseline: echo-only mode

Until general static file serving is implemented, run the harness with `--echo-only`.  The output JSON will contain only the echo workloads and `metadata.echo_only` will be `true`.  Do not report an echo-only result as a full Phase 1 baseline; label it as an echo-only CLI baseline.

## 8. Reproducibility

To make two runs comparable:

- Use the same server executable build and optimization flags.
- Use the same container image, kernel, CPU architecture, mount type, and resource limits (`--cpus`, `--memory`, etc.).
- Use the same `--repeat`, `--port` mode, `--echo-only` flag, and log destination.
- Do not mix sanitizer builds with baseline measurements.

The harness itself keeps the workload definitions, payload generator, and measurement counts fixed.  Changing any of these counts is a protocol change and should be recorded with a new protocol version.

## 8. Relation to open decisions

- D-08 host/image/mount/network/resource-limit contract: recorded per-run in `metadata` and `environment`, not fixed globally in this document.
- D-07 dynamic port / READY format: the harness supports both the current marker and a future `READY` marker.
- D-06 logging schema: server logs are only captured opaquely for debugging; they are not parsed by the benchmark.
- This protocol does not cover Phase 2 concurrency or keep-alive profiles; those will be separate documents.
