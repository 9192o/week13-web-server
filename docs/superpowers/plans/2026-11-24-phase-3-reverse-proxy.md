# Phase 3 Reverse Proxy Implementation Plan

**Goal:** Route to two configured backends and stream HTTP responses without blocking local requests.

**Architecture:** The existing LT epoll loop schedules both endpoints of an exchange. The client session owns that exchange; the HTTP wire module parses and reformats bounded data without owning descriptors.

**Tech Stack:** C17, Linux epoll and sockets, Python loopback integration tests, Linux/amd64 Docker.

**Spec:** `docs/PHASE_3_KICKOFF.md`; the user selected the scope including multiple backends and streaming. The M1 design is a smaller intermediate scope.

## Contracts

- Repeat `--route /prefix/=IPv4:PORT`; match the longest literal prefix ending
  in `/`. Preserve the path/query. An unmatched origin path uses the Phase 2
  router. The client's authority never selects a destination.
- GET, HEAD and POST use existing validated request framing. Rewrite Host
  and Content-Length and remove connection-local fields in both directions.
  One upstream connection per request. Proxied replies close the downstream
  connection; other requests retain the existing persistence rules.
- Bound head and body staging independently. Do not read upstream while a
  staged downstream segment remains unwritten. Support no-body, length,
  chunked and EOF upstream framing. Reframe EOF as chunked downstream so a
  transport failure is distinguishable from successful completion.
- Use monotonic connect, head, and relay deadlines capped at shutdown. Return
  502/504 before the final response starts; close without a second response
  after it starts. Do not release epoll event keys until the batch is drained.

## Tasks

1. Add loopback tests for two destinations, binary POST, slow upstream versus
   local GET, response streaming, connection refusal/timeout, and cleanup.
   Run against `2785e06` to demonstrate the missing behavior.
2. Implement route config and bounded HTTP wire functions. Test normal
   framing and rejection paths directly, without opening sockets.
3. Integrate the nonblocking exchange with the existing session and event
   loop. Run the proxy suite, then the Phase 2 functional and memory gates.
4. Record CLI usage, measured caps, remaining limitations and Docker evidence.
