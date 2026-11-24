# Scoped Phase 2 correctness review

- Scope: sibling worktree `/mnt/c/Users/jin/Desktop/workspace/krafton_study/week13-web-server-phase2`, implementation commit `ed230e3083010219aa186382d7e3f5c358ff49fc`, relative to `5b20a75`.
- codeQualityStatus: WATCH
- recommendation: APPROVE
- blockers: []
- Approval applies to this bounded static correctness review. It does not independently certify every Phase 2 completion criterion.

## CRITICAL

None found.

## HIGH

None found. No actionable correctness blocker was established in normal request state/buffer lifetimes, signal dispatch priority, or deadline handling.

## MEDIUM

1. `tests/test_shutdown.py:27-45`, `tests/test_shutdown.py:99-103`, `tests/test_shutdown.py:144-150`: shutdown tests use fixed listen ports and sleeps to infer that headers have been validated. Their READY helper calls blocking `read1()` without a readiness timeout, so its nominal timeout cannot stop a silent but still-running child. These are test reliability issues: concurrent runs can collide, and scheduling can make a valid request look unvalidated when the signal arrives. Prefer unique ports and an observable protocol barrier such as the existing `100 Continue` pattern; bound readiness reads. This is a programming/remove-ai-slops determinism finding, not a demonstrated server defect.

2. `client_session.c:49-114` and the module overall: the new session module has 807 nonblank, noncomment, nonpreprocessor lines, combining request parsing, response transfer, logging, and session lifecycle. It exceeds both loaded skills' 250 pure LOC criterion without an exception marker. A later maintenance change should separate responsibilities while preserving the current ownership contract. This is a skill-perspective maintainability violation, not a correctness blocker in this review.

## LOW

1. `tests/benchmark_phase2.py:316-327`, `tests/benchmark_phase2.py:394-405`, `tests/benchmark_phase2.py:475-511`: three configured repeats are aggregated into one result per workload/concurrency/condition rather than retained as independent repetition results. The artifact establishes scoped A/B/C observations and errors, but cannot substantiate repeat-to-repeat variability. Preserve per-repeat results before making statistical performance claims.

## Correctness assessment

- Borrowed request-line/header/target slices retain `head_buf` backing through response preparation and transmission. Echo response bytes remain owned by the body parser until response completion. `cs_reset_request()` releases response/file ownership and destroys the previous body parser only after completion, then moves saved pipeline bytes into the next request buffer (`client_session.c:214-246`, `client_session.c:453-494`, `client_session.c:918-936`). I did not find an ordinary request lifetime violation.
- Pipeline surplus is copied before the current backing storage is reused, next requests are handled only after the prior final response completes, and persistence requires a completely consumed request boundary. Early application errors with unfinished bodies close the connection.
- `dispatch_events()` checks the signalfd-backed shutdown state before each client event, including when the signal descriptor is outside the returned event batch, and checks again before accepting queued connections (`epoll_loop.c:178-210`). Clients marked dead during shutdown remain allocated until copied events have been examined. I found no stale event-pointer dereference along this path.
- Shutdown stops listener interest, closes the listener, rejects unvalidated/idle sessions, caps active deadlines, and prevents another pipelined request from starting. Normal body receipt updates the idle deadline under the request total cap; final transmission receives its independent send budget. These paths match the reviewed requirement contracts.

## Evidence inspected

- Read `docs/PHASE_2_REQUIREMENTS.md`, relevant retained Phase 1 deadline requirements, complete `client_session.c`, `epoll_loop.c`, and `shutdown.c`, associated headers, and related production/test diffs. Inspected socket tests for keep-alive, epoll concurrency and shutdown.
- Read `docs/PHASE_2_TESTING.md`, supplied concurrently during review. It records Docker functional and memory exits of 0 using monitor handles `mon_C68NCXNP658P9V05` and `mon_3Z9G9E7F45QA3BNC`. These are reported results: raw monitor output was not available to this child, so I did not independently authenticate those runs.
- Independently parsed `artifacts/phase2/benchmark.json` after the producer completed writing it. It contains 63 result rows. A has 12,914 successful requests and one failure; B and C each have 12,915 successful requests and zero failures. Every C row demonstrates reuse through successful requests exceeding recorded opened connections; `missing_reuse` is empty. Documentation accurately reports the benchmark command's exit 1 and preserves the Phase 1 baseline reset rather than claiming a completely clean benchmark.
- The artifact identifies executable hashes and shared environment details. Source revisions, Docker image identity, resource settings and compiler flags are linked through the verification script and testing document, rather than embedded in the JSON itself. No claim of a universal performance improvement is warranted or made here.
- Python AST parsing passed for `tests/benchmark_phase2.py`, `tests/test_shutdown.py`, `tests/test_epoll.py`, and `tests/test_keepalive.py`, without writing bytecode or changing source files.
- C compiler check: BLOCKED, host `gcc` is unavailable (command exit 127). Language-server inspection: BLOCKED, `clangd` is not installed. No compiler/LSP installation was attempted.
- Runtime tests: NOT_RUN by this child. No existing server binary was present; the scoped review did not create binaries, rerun Docker workflows, or modify the sibling worktree.

## Skill-perspective check

Loaded `remove-ai-slops/SKILL.md`, `programming/SKILL.md`, and the programming testing authoring-gate and junk-class references from the installed 5.1.81 skill directory before judging test relevance or maintainability. Applied their review criteria only; the read-only instruction supersedes their cleanup/refactoring procedures.

The inspected tests exercise real protocol results, connection limits, request ordering, shutdown behavior, and resource recovery. I found no deletion-only test, prose pin, source-grep test, tautology, test that merely certifies a requested removal, or unnecessary production extraction/normalization in the scoped paths. Fixed sleeps/ports and the oversized session module violate the skills as recorded above. Protocol boundary parsing and body-limit validation serve the actual server contract and are not needless production validation.

Only this report artifact was written. No fixes, offensive workflows, or recursive delegation were performed. The toolkit status call returned `ULW_LOOP_PLAN_MISSING`, so the report uses the requested no-plan evidence fallback.
