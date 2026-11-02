# Handoff: Phase 5 Fast network I/O

- Owner: Codex   Dates: 2026-11-02 to 2026-11-02   Branch: `phase-5-epoll-io` (uncommitted)
- Status: Implementation and regression checks are available. Phase 5 acceptance is **not signed off**: the raw packet capture is blocked by environment permissions, and the 1,000-active-request and high-load overhead targets were not reached.

## What now exists

- `src/net/epoll_forwarder.*`: one level-triggered epoll reactor; nonblocking, close-on-exec upstream sockets; TCP_NODELAY; forward-proxy HTTP requests; per-proxy keep-alive pool; stale-peer detection; connect/response deadlines; global and per-proxy descriptor limits; oldest-idle eviction under global pressure.
- `src/main.cpp`: `forwarder: "epoll"` selects `EpollForwarder`; `simple` remains the default reference implementation.
- `src/common/config.*`: `forwarder_pool.max_connections_per_proxy` (default 8), `max_total_connections` (default 128), and `idle_timeout_ms` (default 30000).
- `src/common/metrics.*` and `src/admin/admin_server.cpp`: upstream opened/reused, byte and timeout counters; active/idle socket gauges; bounded `gateway_overhead_ms` percentiles in `/api/stats` and `/metrics`.
- `tests/test_forwarder.cpp`: keep-alive reuse, concurrent pool cap, idle eviction, peer-close recovery, timeout mapping, global cap eviction and descriptor balance coverage.
- `bench/phase5_benchmark.py`: direct target/proxy baselines, simple-vs-epoll latency and throughput comparison, memory and concurrency sampling, and an optional tcpdump attempt.
- `bench/results/phase5_performance.md` and `.json`: recorded benchmark run. `bench/results/phase5_reuse_syscalls.txt` supplements the unavailable packet capture with one upstream `connect()` and two request writes on the same descriptor.
- `testbed/proxies/faultproxy.py`: enables TCP_NODELAY on accepted proxy sockets to remove a measured delayed-ACK artifact from persistent responses.

## How to build, run and test

```bash
cmake -S . -B build
cmake --build build -j
./build/pg_tests

cmake -S . -B build-asan -DPG_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
./build-asan/pg_tests
```

Set `"forwarder": "epoll"` in a local gateway config to run the event-driven forwarder. The full benchmark creates temporary configs with both `simple` and `epoll`, then exercises the local test bed:

```bash
python3 bench/phase5_benchmark.py
```

The benchmark writes JSON and Markdown results under `bench/results/`. It attempts a loopback tcpdump capture; this environment denied capture on `lo`, so the script records that limitation and continues the measurements.

## Demo command and expected result

```bash
python3 bench/phase5_benchmark.py
```

Expected: the local target and proxies start, all benchmark requests return HTTP 200, `epoll` reports opened and reused upstream sockets, the 1,000-client batch completes, and results are written to `bench/results/phase5_performance.md` and `.json`. Check `testbed/run_local.sh status` afterward; the benchmark stops the local services in its cleanup path.

## Acceptance results

| Handbook check | Result | Evidence / note |
| --- | --- | --- |
| One upstream connection serves multiple requests | Supplemental pass | Syscall trace shows one upstream `connect()` and two request writes on the same fd. Raw SYN/SYN-ACK/ACK packet trace remains blocked by I-014. |
| Keep-alive latency below connection-per-request | Pass in measured local comparison | 1,000 sequential requests: epoll p95 1.86 ms; simple p95 2.19 ms. Direct keep-alive proxy p95 was 1.20 ms. |
| Gateway overhead p95 <= 5 ms | Partial | `GatewayCore` overhead p95 0.16 ms at concurrency 32; 22.43 ms in the 1,000-client trial. The metric excludes pre-core queueing and client response writes. |
| Throughput >= 5,000 requests/s | Measured, target missed | Epoll 1,448 requests/s at concurrency 32; simple 1,005 requests/s. Direct target was 2,705 requests/s and direct proxy 1,468 requests/s; the Python test bed is a material limit. |
| 1,000 in-flight requests | Measured, target missed | 1,000 clients completed; observed peak active workers was 718. |
| Memory <= 256 MiB at 1,000 concurrent requests | Partial | Peak gateway RSS was 29.7 MiB in the 1,000-client trial, but the run peaked at 718 active workers rather than 1,000. Unit coverage confirmed FD count returns to baseline after forwarder teardown. |
| Phase 1–4 tests with epoll | Partial | Unit suite, Phase 2 rate/AIMD, Phase 3 retry and Phase 4 fault verifiers passed with epoll-selected configs; epoll smoke passed. The full Phase 1 acceptance script did not pass its 1,000-request assertion: 944/1,000 succeeded because Proxy 3 returned injected HTTP 502s, which are passed through without retry under D-021. The Phase 1 script's `requests_failed == 0` assertion therefore also failed. |
| ASan/UBSan | Pass | 93 cases / 1,489 assertions passed. |
| Decisions recorded | Pass | D-029 through D-035. |

## Interfaces added or changed

- `Forwarder`: `EpollForwarder` implements the existing interface without changes to scheduler, routing or health modules.
- Configuration: `forwarder_pool.*` keys and defaults are documented in `docs/INTERFACES.md`.
- `MetricsSink`: added signed gauge updates and gateway-overhead samples. Admin output includes upstream pool counters/gauges and `gateway_overhead_ms` percentiles.
- Full field and config definitions: `docs/INTERFACES.md`.

## Decisions made

- D-029: keep ingress HTTP-only and defer CONNECT tunneling.
- D-030: one level-triggered reactor and bounded keep-alive pool.
- D-031: shared upstream pool metrics.
- D-032: configurable pool bounds and global descriptor cap.
- D-033: separate GatewayCore overhead percentiles.
- D-034: TCP_NODELAY on test proxy sockets for valid persistent-response measurements.
- D-035: evict oldest idle socket when global capacity is needed.

## Known problems and shortcuts

- I-001: CONNECT/HTTPS remains deferred; HTTP-only ingress is the agreed Phase 5 scope.
- I-004: client-side connections still close after each response; this phase pools gateway-to-proxy connections.
- I-007: benchmarked throughput is below 5,000 requests/s; Python target/proxy limits were recorded.
- I-014: raw tcpdump capture cannot run in this environment because loopback capture permission is unavailable. A syscall trace is supplemental evidence, not a packet capture.
- The high-load run did not reach 1,000 active workers and exceeded the 5 ms overhead target. Performance sign-off remains open; see `bench/results/phase5_performance.md` and `docs/ISSUES.md`.
- The full Phase 1 acceptance script was rerun with epoll: direct test-bed fault checks passed (Proxy 3 measured 18% injected failures), but 56 requests received that proxy's HTTP 502 and were not retried, as specified by D-021. Other Phase 1 checks in that run passed, including malformed input, survival, admin shape and FD balance (8 before, 10 after; within the script's +2 allowance).

## First things the next owner should do

1. Review this handoff and `bench/results/phase5_performance.md`; Phase 5 is not signed off yet.
2. On a host with `CAP_NET_RAW` or `CAP_NET_ADMIN`, rerun the benchmark and save the required loopback tcpdump trace.
3. Investigate the 718-worker concurrency ceiling and 22.43 ms p95 GatewayCore overhead under the 1,000-client load. Repeat against a faster non-Python target/proxy if available, and record results even if targets remain unmet.
4. After the team accepts the measured limits and capture evidence, complete the Phase 5 done checklist and handoff before starting Phase 6.
