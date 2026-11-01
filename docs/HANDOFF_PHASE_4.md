# Handoff: Phase 4 Health and fault tolerance

- Owner: Codex
- Date: 2026-11-02
- Branch: `phase-4-health`
- Status: Phase 4 done; next phase is Phase 5 (Fast network I/O).

## Delivered

- `src/health/health_state.*`: bounded rolling success/RTT samples and a circuit breaker with CLOSED, OPEN and HALF_OPEN transitions.
- `src/health/health_monitor.*`: one fixed background thread per configured proxy. It probes `GET http://test.example/health` through the existing `Forwarder` every 2 seconds, with probe connect/response deadlines capped at 1 second. Network I/O runs outside proxy-state locks.
- `GatewayCore` integrates real request and probe outcomes into the existing proxy table and snapshots. The default rolling window is 20 outcomes; reported rolling success/RTT includes traffic and probes. `healthy` derives from the latest four outcomes (or the full smaller configured window), and is false below 50% success.
- Configurable circuit threshold and cooldown now drive the existing state table. Five consecutive failures open the circuit by default; after the 10 second cooldown a single monitor probe is admitted in HALF_OPEN. A successful probe closes and resets the breaker; failure reopens it and restarts cooldown.
- The router admits only healthy CLOSED proxies. Normal traffic cannot use OPEN or HALF_OPEN proxies.
- `/proxies`, `/api/stats` and `/metrics` expose circuit, health, probe, rolling RTT, last-probe and open-count fields/counters. Probe totals are separate from request totals and per-proxy traffic attempt counts.
- `tests/test_health.cpp` covers rolling-window behavior, health derivation/recovery, circuit transitions, cooldown with `FakeClock`, single HALF_OPEN test gating, router exclusion and GatewayCore probe recovery. Admin/config tests cover live fields and configuration.
- `bench/phase4_fault_verify.py` and `bench/results/phase4_fault_injection.md` record kill/restart and delay injection evidence.

## Decisions

D-024 to D-028 document rolling statistics and health rules, probe design, circuit behavior, probe counter separation, bounded probe threads/deadlines, health recovery derivation, and Phase 2 verifier isolation.

## Verification

- `cmake -S . -B build && cmake --build build -j`: passed.
- `./build/pg_tests`: 88 test cases / 1,401 assertions passed.
- `scripts/smoke.sh --local`: passed.
- `python3 bench/phase2_verify.py`: passed; site A max 50/s, site B 100 requests, AIMD 8 -> 4 -> 6. D-028 keeps this check isolated from circuit cooldown.
- `python3 bench/phase3_retry_verify.py`: passed; Proxy 3 timed out at 500 ms, retry via Proxy 1 returned 200.
- `cmake -S . -B build-asan -DPG_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-asan -j && ./build-asan/pg_tests`: passed, 88 test cases / 1,401 assertions; no sanitizer reports.
- `python3 bench/phase4_fault_verify.py`: passed. Proxy 3 was detected OPEN in 3.80 s; 100/100 outage requests succeeded, with zero assigned to OPEN Proxy 3. After restart it traversed OPEN -> HALF_OPEN -> CLOSED in 9.42 s. Proxy 2's injected 1,500 ms delay was detected in 4.25 s and it returned CLOSED 11.64 s after delay removal. Full measurements are in `bench/results/phase4_fault_injection.md`.

Socket-based validation required loopback access outside the restricted sandbox; those commands passed with that access.

## Phase 4 done checklist

- [x] With Proxy 3 killed, no requests are assigned to it after OPEN; measured zero of 100.
- [x] Failed proxy detection within 10 seconds; measured 3.80 seconds.
- [x] With one of three proxies killed mid-run, at least 99% success; measured 100/100.
- [x] Restart recovery passes through HALF_OPEN and returns to CLOSED.
- [x] Circuit state and related health fields are visible through `/proxies` and `/metrics`.
- [x] Unit suite passes normally and with ASan/UBSan.
- [x] Rolling window, health rule, probe design and combined outcome behavior are recorded in D-024 through D-027.
- [x] Handoff written and tracker updated.

## Known follow-up and limits

- I-013 remains open: the SRS success definition is unavailable in this checkout. D-010 remains the established definition and was not changed; reconcile with the SRS at team follow-up.
- Health probes use the test-bed `test.example` alias and `/health` endpoint. A deployment using different controlled health URLs will need a later config/interface extension.
- The health window and probe cadence are configurable as `circuit.window_size` and `circuit.probe_interval_ms`; the routing health flag uses the most recent four outcomes (or the full smaller window).

## Resume / current state

```bash
git status --short --branch && cmake -S . -B build && cmake --build build -j && ./build/pg_tests
```
