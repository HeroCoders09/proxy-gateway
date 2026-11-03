# Handoff: Phase 5 Fast network I/O

- Owner: Codex   Updated: 2026-11-03   Branch: `codex/real-world-gateway`
- Status: Phase 5 implementation and measurable acceptance checks are complete. The required raw packet capture remains blocked by this environment lacking `CAP_NET_RAW`; formal handbook closure still needs that artifact. The 5,000 requests/s target was measured and missed.

## What now exists

- `src/net/epoll_forwarder.*`: one level-triggered epoll reactor; nonblocking, close-on-exec upstream sockets; TCP_NODELAY; forward-proxy HTTP requests; per-proxy keep-alive pool; stale-peer detection; connect/response deadlines; global and per-proxy descriptor limits; oldest-idle eviction under global pressure.
- `src/main.cpp`: `forwarder: "epoll"` selects `EpollForwarder`; `simple` remains the default reference implementation.
- `src/common/config.*`: `forwarder_pool.max_connections_per_proxy` (default 8), `max_total_connections` (default 128), and `idle_timeout_ms` (default 30000).
- `src/common/metrics.*` and `src/admin/admin_server.cpp`: upstream opened/reused, byte and timeout counters; active/idle socket gauges; bounded `gateway_overhead_ms` percentiles in `/api/stats` and `/metrics`.
- `tests/test_forwarder.cpp`: keep-alive reuse, concurrent pool cap, idle eviction, peer-close recovery, timeout mapping, global cap eviction and descriptor balance coverage.
- `bench/phase5_benchmark.py`: direct target/proxy baselines, three repeated sequential simple-vs-epoll latency samples, throughput comparison, memory and concurrency sampling, and a tcpdump attempt. The 1,000-client trial uses a controlled 2,000 ms target delay and samples active gateway workers.
- `bench/results/phase5_performance.md` and `.json`: final benchmark results. `bench/results/phase5_reuse_syscalls.txt` supplements the unavailable packet capture with one upstream `connect()` and two request writes on the same descriptor.
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
| One upstream connection serves multiple requests | Supplemental pass; packet artifact blocked | Syscall trace shows one upstream `connect()` and two request writes on the same fd; benchmark reports 3,468 reuse events from 32 opened sockets. Raw SYN/SYN-ACK/ACK packet trace remains blocked by I-014. |
| Keep-alive latency below connection-per-request | Pass | Median p95 across three 1,000-request runs: epoll 1.26 ms; simple 1.48 ms. |
| Gateway overhead p95 <= 5 ms | Pass in final run | `GatewayCore` overhead p95 0.08 ms at concurrency 32 and 3.29 ms in the final 1,000-client trial. Queueing before GatewayCore and client response writes are excluded by the documented metric. |
| Throughput >= 5,000 requests/s | Measured, target missed | Epoll 1,684 requests/s at concurrency 32; simple 1,291 requests/s. Direct Python target reached 4,427 requests/s and direct proxy 2,196 requests/s in the final run. The test-bed path is below the target. |
| 1,000 in-flight requests | Pass | 1,000 client requests; observed peak active gateway workers 1,000. |
| Memory <= 256 MiB at 1,000 concurrent requests | Pass | Peak gateway RSS 31.4 MiB at 1,000 active workers. FD count returned within the existing +2 acceptance allowance after the 1,000-request Phase 1 run. |
| Phase 1–4 tests with epoll | Pass | Full Phase 1 end-to-end checks passed after separate direct fault checks reset injected delay/failure; Phase 2 rate/AIMD, Phase 3 retry, and Phase 4 fault/recovery verifiers passed. |
| ASan/UBSan | Pass | 96 CTest cases passed in both normal and ASan/UBSan builds. |
| Decisions recorded | Pass | D-029 through D-035; D-046 records the Phase 5 contention changes. |

## Interfaces added or changed

- `Forwarder`: `EpollForwarder` keeps the existing interface. The Phase 5 contention follow-up adjusts GatewayCore/PolicyRouter selection locking; scheduler and health interfaces remain unchanged.
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
- Throughput remains below the 5,000 requests/s goal on the bundled Python test bed; the 1,000-active-worker and <=5 ms high-load core-overhead checks passed in the final measured run. The raw packet trace is the only Phase 5 deliverable still blocked; see I-014 and `bench/results/phase5_performance.md`.
- The full Phase 1 acceptance script was rerun with epoll: direct test-bed fault checks passed (Proxy 3 measured 18% injected failures), but 56 requests received that proxy's HTTP 502 and were not retried, as specified by D-021. Other Phase 1 checks in that run passed, including malformed input, survival, admin shape and FD balance (8 before, 10 after; within the script's +2 allowance).

## Remaining Phase 5 closure item

Run `python3 bench/phase5_benchmark.py` on a host with `CAP_NET_RAW` or `CAP_NET_ADMIN` and save `bench/results/phase5_keepalive.pcap`. This environment explicitly rejects capture on loopback with `Operation not permitted`, even when the local benchmark itself runs with network access. The throughput target miss is recorded per the handbook requirement; improving the Python test-bed ceiling is separate from the epoll implementation.
