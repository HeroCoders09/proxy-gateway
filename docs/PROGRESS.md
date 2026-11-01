# Progress log

Newest entries at the bottom of the Log. Times are UTC.

## Current state (keep this block up to date)
- Current phase: 3 done. Next: Phase 4
- Current owner: Phase 3 owner
- Branch: `phase-3-Routing`
- Last green build and test run: 2026-11-01 18:57 UTC (82 test cases / 1,345 assertions, normal and ASan/UBSan)
- Command that shows current state: `git status --short --branch && cmake -S . -B build && cmake --build build -j && ./build/pg_tests`

### Phase status tracker (mirror of handbook 10.3)

| Phase | Name | Owner | Status | Handoff file |
| --- | --- | --- | --- | --- |
| 1 | Foundation and skeleton | Gourav + Claude agent | Done (tag `phase-1-done`) | `docs/HANDOFF_PHASE_1.md` |
| 2 | Scheduling and concurrency | Phase 2 owner | Done | `docs/HANDOFF_PHASE_2.md` |
| 3 | Routing and retries | Phase 3 owner | Done | `docs/HANDOFF_PHASE_3.md` |
| 4 | Health and fault tolerance |  | Not started | `docs/HANDOFF_PHASE_4.md` |
| 5 | Fast network I/O |  | Not started | `docs/HANDOFF_PHASE_5.md` |
| 6 | Dashboard, metrics, benchmarks, demo |  | Not started | `docs/HANDOFF_PHASE_6.md` |

## Open questions
- Is HTTPS through the gateway (HTTP CONNECT) required? Today it returns 400 (I-001). Ask the team before Phase 5. Asked to: team, on 2026-11-01.
- Does the SRS define "successful request" differently from D-010 (transport OK and status below 500)? UNVERIFIED; the SRS text is not in this checkout. Carry to the next team sync before final acceptance.
- Phase owner names in the handbook (Persons 1 to 6) are still blank.

## Log
- 2026-11-01 18:43 UTC Phase 3 owner: Read the requested tracking files and handbook Sections 2/3/5. Baseline build and normal tests passed (74 cases / 1,267 assertions), local smoke passed, `bench/phase2_verify.py` passed (49/s site A, 100 site B, AIMD 8 -> 4 -> 6), and ASan/UBSan tests passed. Initial smoke was invalidated by a pre-existing listener on :8000; after stopping that listener (per recorded prior authorization), the required chain passed. No code was changed before the baseline passed.
- 2026-11-01 18:43 UTC Phase 3 owner: Added D-020 for routing score normalization, RTT EMA (alpha 0.2), eligibility, two retry limit and GatewayCore retry ownership. Implemented the first draft of `PolicyRouter` and wired state snapshots, active reservations, outcome feedback, and retries into `GatewayCore`; normal build is in progress. Remaining: tests, admin/live proxy visibility, benchmark comparison/results, docs/interface/issues updates, and full normal/sanitizer validation.
- 2026-11-01 18:43 UTC Phase 3 owner: `PolicyRouter` compiled cleanly. Initial test compile found the vendored Catch2 uses the amalgamated header (`catch_amalgamated.hpp`) rather than modular Catch2 includes; corrected the new test include. Rebuilding now.
- 2026-11-01 18:47 UTC Phase 3 owner: Added routing/retry tests (79 cases / 1,301 assertions pass). A one-proxy retry exhaustion initially hid a prior timeout as 502; D-021 records the correction. Added `bench/phase3_compare.py`; five-policy run and recorded results are next. Full Phase 3 checklist and sanitizer validation remain.
- 2026-11-01 18:51 UTC Phase 3 owner: All five policies ran against 100 requests at concurrency 8 and the result was written to `bench/results/phase3_routing.csv`; interpretation is in `bench/results/phase3_routing.md`. Initial comparison revealed latency cold-start starvation, so D-022 adds exploration for proxies without RTT samples. After that change, the run measured leastconn highest throughput (482 req/s) and lowest p50 (5.43 ms); sample is explicitly marked noisy and inconclusive for success-rate ranking. Added a deterministic leastconn reservation-balance test. Full final validation remains.
- 2026-11-01 18:52 UTC Phase 3 owner: Final normal suite passed (81 cases / 1,335 assertions) and local smoke passed. `bench/phase2_verify.py` then failed because its three-proxy setup made the intentionally failing Proxy 3 selectable. D-023 records a minimal verifier-only change to restrict its temporary config to the normal proxy, isolating Phase 2 scheduler/AIMD checks. Rerunning the Phase 2 verifier and sanitizer validation next.
- 2026-11-01 18:57 UTC Phase 3 owner: Final validation passed: 82 test cases / 1,345 assertions normally and under ASan/UBSan; local smoke passed; Phase 2 rate/AIMD verifier passed (50/s site A, 100 site B, AIMD 8 -> 4 -> 6). Added live Phase 3 retry/no-route/policy-selection counters and per-proxy requests/successes/failures/active/RTT to `/api/stats`, `/proxies` and `/metrics`; admin unit test passes.
- 2026-11-01 18:58 UTC Phase 3 owner: `bench/phase3_retry_verify.py` passed an end-to-end local test: Proxy 3 timed out at the configured 500 ms response deadline, the gateway retried via proxy 1, returned 200, and recorded one retry with no active-count leak. Phase 3 checklist is complete; handoff written. SRS success-definition reconciliation remains UNVERIFIED because the source SRS is not present in this checkout.
- 2026-11-01 11:32 to 11:41 (earlier working session, times from file dates): Repository skeleton, CMake with sanitizer option, shared structs and interfaces, config loader and CLI, ingress parser and listener, simple forwarder, stub admin server and unit tests were written. Decisions D-001 to D-010 were not written down at the time; they were reconstructed into `docs/DECISIONS.md` afterwards.
- 2026-11-01 15:54 Claude agent: Read the handbook, inspected the existing code. Built it: 70 unit tests passed. Gaps found: no `docs/`, no `testbed/`, no scripts, no end-to-end checks.
- 2026-11-01 15:56 Claude agent: Wrote the test bed: `testbed/target/server.py`, `testbed/proxies/faultproxy.py`, Dockerfiles, `docker-compose.yml`, `testbed/run_local.sh`, `testbed/verify_testbed.sh`, configs `gateway.local.json` and `gateway.docker.json` (D-011 to D-014). No Docker daemon was available, so the compose file is only syntax-checked (I-002).
- 2026-11-01 15:57 Claude agent: Verified test-bed faults directly in local mode: proxy1 about 2 ms, proxy2 about 100 ms slower, proxy3 14.5 to 21 percent failures (200 requests per run).
- 2026-11-01 15:58 Claude agent: Wrote `scripts/smoke.sh`, `scripts/lib.sh`, `tests/e2e/run_phase1_acceptance.sh` and `tests/e2e/phase1_checks.py`. First run: all checks passed except one harness error on an oversized request (the reset after the 400 made my Python client raise).
- 2026-11-01 16:00 Claude agent: Added a lingering close after 400 replies in `src/ingress/listener.cpp` and a unit test for it (D-017). Confirmed the test fails without the fix (20 of 20 connections reset) and passes with it. Fixed the Python client to keep data received before a reset.
- 2026-11-01 16:02 Claude agent: Sanitizer build (`-DPG_SANITIZE=ON`): all unit tests pass. Ran the full acceptance script against the sanitizer-built gateway: all pass, no sanitizer reports in the gateway log.
- 2026-11-01 16:04 Claude agent: Wrote `docs/` (all six tracking files), `README.md`, `.gitignore`. Committed on `phase-1-foundation`.
- 2026-11-01 16:06 Claude agent: Clean-clone check: build with no warnings, 71 test cases pass (normal and sanitizer builds), acceptance script passes, smoke test passes. Wrote `docs/HANDOFF_PHASE_1.md`.

- 2026-11-01 16:20 Gourav (owner's laptop, GCC 11.4, Ubuntu 22.04): `cmake ... && ./build/pg_tests` built with no warnings and passed 71 test cases / 1,250 assertions.
- 2026-11-01 16:22 Gourav + Claude agent: First Docker run failed on a host port clash (8080 in use). Target host port moved to 18080 and made configurable (D-018); `verify_testbed.sh` now fails fast when the test bed is down.
- 2026-11-01 16:30 Gourav: Docker run passed: all containers Healthy about 4 s, `scripts/smoke.sh` SMOKE PASS in Docker mode, `testbed/verify_testbed.sh` all PASS against the containers (proxy2 +100.9 ms, proxy3 17.5 percent failures, proxy1 0 percent). I-002 closed. Merged to `main`, tagged `phase-1-done`.
- 2026-11-01 22:58 Phase 2 owner: Baseline build, 71 tests and local smoke passed after a user-authorized stop of an unrelated process holding :8000. Added per-domain token buckets/FIFO scheduler, async client-FD ownership transfer, fixed bounded worker pool and AIMD (D-019). Normal and ASan/UBSan tests pass: 74 cases / 1,267 assertions; local smoke passes.
- 2026-11-01 23:02 Phase 2 owner: Added live Phase 2 admin metrics and `bench/phase2_verify.py`. Local load/fault verification passed: site A max 50/s, site B 100 requests; AIMD 8 -> 4 -> 6 after injected failures and recovery.

## Stop entry (write this before stopping)
- Done: Phase 3 routing/retry checklist complete; handoff is `docs/HANDOFF_PHASE_3.md`.
- Half done: Nothing in Phase 3.
- Next steps: Phase 4 starts with `docs/HANDOFF_PHASE_3.md`; integrate the health monitor and circuit breaker with the existing proxy table and eligibility filter.
- Known problems: The SRS success-definition question is UNVERIFIED because the SRS is not in this checkout. See `docs/ISSUES.md` for other open items.
- How to resume / see current state: `git status --short --branch && cmake -S . -B build && cmake --build build -j && ./build/pg_tests`
