# Handoff: Phase 3 Routing and retries

- Owner: Phase 3 owner
- Date: 2026-11-01
- Branch: `phase-3-Routing`
- Status: Phase 3 done; next phase is Phase 4 (Health and fault tolerance).

## Delivered

- `src/routing/policy_router.*`: thread-safe `PolicyRouter` implementing `random`, `roundrobin`, `leastconn`, `latency`, and `weighted` behind the shared `Router` interface.
- Eligibility excludes unhealthy proxies, OPEN circuits and proxies already listed in `Job::tried_proxies`. Latency mode explores proxies without an RTT measurement before selecting minimum measured RTT.
- `GatewayCore` owns a thread-safe proxy state table, consistent snapshots, atomic selection plus active-load reservation, outcome feedback and RTT EMA (alpha 0.2). An RAII completion guard decrements active counts on every forwarding return path.
- Transport errors retry through a different eligible proxy, at most twice. HTTP responses (including 5xx) pass through without retry. If no alternate route remains after an error, the last attempt's normal 502/504 response and proxy ID are preserved.
- Phase 3 counters: retries, no-route and selections per policy. `/proxies`, `/api/stats` and `/metrics` expose live per-proxy attempts, successes, failures, active count and RTT.
- `bench/phase3_compare.py` plus results in `bench/results/phase3_routing.csv` and interpretation in `bench/results/phase3_routing.md`.
- `bench/phase3_retry_verify.py` exercises an actual Proxy 3 timeout and successful retry through another local proxy.
- Updated `bench/phase2_verify.py` to use one normal proxy in its temporary config, isolating scheduler/AIMD regressions after Phase 3 routing became active (D-023).

## Decisions

D-020 to D-023 are recorded in `docs/DECISIONS.md`: weighted-score normalization and RTT smoothing, retry ownership, error behavior when the retry pool is exhausted, latency cold-start exploration, and Phase 2 verifier isolation.

## Verification

Final validation on GCC 13.3:

```bash
cmake -S . -B build
cmake --build build -j
./build/pg_tests
scripts/smoke.sh --local
python3 bench/phase2_verify.py
cmake -S . -B build-asan -DPG_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
./build-asan/pg_tests
python3 bench/phase3_retry_verify.py
python3 bench/phase3_compare.py --requests 100 --concurrency 8 --output bench/results/phase3_routing.csv
```

Results: 82 test cases / 1,345 assertions passed normally and with ASan/UBSan; local smoke passed; Phase 2 verifier passed (site A max 50/s, site B 100 requests, AIMD 8 -> 4 -> 6). The Phase 3 retry integration passed: Proxy 3 timed out after 500 ms, retry through proxy 1 returned 200, one retry was counted, and proxy active counts returned to zero.

The five-policy comparison uses one 100-request sample per policy at concurrency 8. Least-connections had highest throughput (482 req/s) and lowest p50 (5.43 ms) in that run. Success rates ranged 0.85 to 0.92 and are inconclusive at this sample size. See the results report for all values and caveats.

## Phase 3 done checklist

- [x] Policy selected at startup through `--policy`, demonstrated by the comparison script.
- [x] Ineligible (unhealthy / OPEN) proxies and tried proxies are excluded by unit tests.
- [x] Least-connections reservations balance within one across 30 simulated concurrent reservations; GatewayCore tests confirm active counts are released.
- [x] A 500 ms timeout on Proxy 3 retries through proxy 1 and succeeds; no more than two retries are allowed.
- [x] Policy results and a short interpretation are recorded.
- [x] Normal and sanitizer unit suites pass.
- [x] Decisions, interfaces, progress and issues are updated.

## Known limitations and next work

- The local proxy's injected failure is an HTTP 502 response; by D-020, HTTP responses pass through without retry. The retry integration therefore adds a temporary delay to Proxy 3 to exercise a transport timeout.
- The SRS success definition has not been checked against D-010 because the SRS text is absent from this checkout (I-013). Confirm at the next team sync before final acceptance.
- Circuit and health fields default to healthy/CLOSED. Phase 4 should drive those fields and use the existing eligibility filter; it should not need to replace the router.
- The recorded policy benchmark is a small noisy demonstration sample, not a performance target claim.

## First steps for Phase 4

1. Read `docs/HANDOFF_PHASE_3.md`, then the current tracking docs.
2. Build the health monitor and circuit breaker around `GatewayCore::report` / `HealthReporter` and the proxy snapshot interface.
3. Ensure HALF_OPEN's single-test gate is enforced before making HALF_OPEN proxies generally eligible; the current Phase 3 filter only excludes OPEN, as specified.
4. Run the full verification chain above and add Phase 4 fault-injection evidence.

Current-state command: `git status --short --branch && cmake -S . -B build && cmake --build build -j && ./build/pg_tests`
