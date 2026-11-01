# Progress log

Newest entries at the bottom of the Log. Times are UTC.

## Current state (keep this block up to date)
- Current phase: 1 done. Next: Phase 2
- Current owner: Phase 1 owner (Gourav, with a Claude agent)
- Branch: merged to `main`, tagged `phase-1-done` (D-003)
- Last green build and test run: 2026-11-01 16:06 (clean clone: build with no warnings, 71 test cases / 1,250 assertions pass, sanitizer build passes, acceptance script passes)
- Command that shows current state: `cmake -S . -B build && cmake --build build -j && ./build/pg_tests && tests/e2e/run_phase1_acceptance.sh --local`

### Phase status tracker (mirror of handbook 10.3)

| Phase | Name | Owner | Status | Handoff file |
| --- | --- | --- | --- | --- |
| 1 | Foundation and skeleton | Gourav + Claude agent | Done (tag `phase-1-done`) | `docs/HANDOFF_PHASE_1.md` |
| 2 | Scheduling and concurrency |  | Not started | `docs/HANDOFF_PHASE_2.md` |
| 3 | Routing and retries |  | Not started | `docs/HANDOFF_PHASE_3.md` |
| 4 | Health and fault tolerance |  | Not started | `docs/HANDOFF_PHASE_4.md` |
| 5 | Fast network I/O |  | Not started | `docs/HANDOFF_PHASE_5.md` |
| 6 | Dashboard, metrics, benchmarks, demo |  | Not started | `docs/HANDOFF_PHASE_6.md` |

## Open questions
- Is HTTPS through the gateway (HTTP CONNECT) required? Today it returns 400 (I-001). Ask the team before Phase 5. Asked to: team, on 2026-11-01.
- Does the SRS define "successful request" differently from D-010 (transport OK and status below 500)? Not checked against the SRS text. Ask the team before Phase 3.
- Phase owner names in the handbook (Persons 1 to 6) are still blank.

## Log
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

## Stop entry (write this before stopping)
- Done: All of Phase 1 (handbook 4.2 tasks 1 to 9, deliverables 4.3, every item of the done checklist 4.5). Evidence in `docs/HANDOFF_PHASE_1.md`.
- Half done: Nothing in Phase 1. The optional gateway container (`testbed/gateway/Dockerfile`, compose profile `gateway`) has never been built (I-010).
- Next steps, in order: 1. Phase 2 owner reads `docs/HANDOFF_PHASE_1.md`, builds, runs tests and `scripts/smoke.sh`. 2. Create branch `phase-2-scheduling`. 3. Replace the synchronous ingress handler with per-domain queues, token bucket, bounded worker pool and AIMD (handbook section 5).
- Known problems: see `docs/ISSUES.md` (open: I-001, I-003 to I-012 except I-002; I-010 partly fixed).
- How to resume: `git checkout main && cmake -S . -B build && cmake --build build -j && ./build/pg_tests && scripts/smoke.sh`
