# Handoff: Phase 6 Dashboard, metrics, benchmarks and demo

- Owner: Codex   Updated: 2026-11-03   Branch: `phase-6-dashboard`
- Status: Implementation present; Phase 6 acceptance and demo rehearsal are not signed off. Changes are uncommitted for the repository owner to review and commit.

## Implemented so far

- `dashboard/index.html`: light analytics dashboard for RPS, success, latency percentiles, AIMD, queues, proxy health/circuit state, and recent requests; no left navigation rail. Controls cover policy/rate, health awareness, per-proxy test-bed delay/failure settings and presets, load generation and the benchmark. Proxy host/port values are visible but read-only.
- `src/output/result_writer.*`: bounded asynchronous CSV/JSONL writer; last 50 rows are available through `/api/recent`; URLs are redacted before display or persistence; output drops are counted.
- `src/admin/`: live API reports `stub:false`, active policy, health-awareness state, sampled RPS, recent requests and settings controls. Admin bind remains config-controlled and localhost by default.
- `src/gateway/` and `src/scheduler/`: runtime policy switch and default rate/burst update.
- `testbed/control-service/control_service.py`: localhost-only allow-listed proxy actions and benchmark/load-generator launch.
- `bench/phase6_benchmark.py`: policy, pool size, fixed/AIMD, simple/epoll keep-alive and fault-injection comparisons; writes CSV plus interpretation markdown and records counter accuracy.
- `bench/load_generator.py`, `scripts/demo.sh`, `config/gateway.demo.json`, and `docs/DEMO.md`.
- D-036 to D-041 record output, runtime controls, control-service, specification and proxy-scenario decisions.

## Checks run

- `cmake -S . -B build && cmake --build build -j`: passed before the latest dashboard proxy-scenario edits; test binary built, not executed. Rebuild against the current worktree before handoff.
- Python `py_compile`, dashboard JavaScript `node --check`, and `git diff --check`: passed after the latest dashboard proxy-scenario edits.
- Unit tests, acceptance scripts, dashboard runtime, fault controls, benchmark suite, metrics accuracy, sanitizers and two demo rehearsals: not run yet.

## Remaining before Phase 6 sign-off

1. Rebuild current worktree, then run the unit and sanitizer suites; the old Phase 1 `stub` assertion was updated to expect live data.
2. Run local demo and verify admin endpoints, dynamic policy/rate controls, result output and live charts.
3. Run fault buttons in local and Docker test-bed modes where available; inspect circuit recovery.
4. Run `python3 bench/phase6_benchmark.py`; review the CSV/markdown and confirm all counter deltas are within 1% or explain discrepancies.
5. Rehearse `docs/DEMO.md` twice from clean starts and fill in the rehearsal record.
6. Update the editable SRS and Design Document source. The supplied Design Document is a PDF; the SRS and editable design source are not in this checkout. See D-039.

## Phase 5 dependency status

Phase 5 implementation is available, but its handoff lists unmet high-load throughput/concurrency/overhead targets and unavailable raw tcpdump evidence. Phase 6 work is on `phase-6-dashboard`; this branch was created from the Phase 5 work, and no Phase 5 acceptance claim is made. Revisit those acceptance issues before final project sign-off.

## Scope notes

- The scenario editor controls only the three local test-bed proxies, with bounded delay/failure injection and fixed presets. No external proxy-provider credentials or arbitrary endpoint editing are part of this implementation.
- The supplied Design Document is a PDF and the editable SRS/design sources are not in this checkout. D-039 records that specification updates remain needed before final release.
- No commit or push has been made. The repository owner will review and commit the branch.

## Resume

```bash
git status --short --branch
cmake -S . -B build
cmake --build build -j
./build/pg_tests
```
