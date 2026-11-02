# Phase 5 performance results

Run date: 2026-11-02 15:58:39 IST. Local test bed: Python target and fault proxies on loopback; gateway compiled with the current workspace build.

## Measurements

- Direct target throughput: 2705.3 requests/s (500 requests, concurrency 32).
- Direct proxy throughput (new client connection per request): 1468.4 requests/s.
- Direct proxy keep-alive: p50 0.69 ms, p95 1.20 ms.
- Gateway simple: p95 latency 2.19 ms; throughput 1004.7 requests/s at concurrency 32.
- Gateway epoll: p95 latency 1.86 ms; throughput 1447.5 requests/s at concurrency 32.
- Epoll GatewayCore overhead: p95 0.16 ms at concurrency 32; p95 22.43 ms during the 1,000-request trial.
- Epoll reuse: 32 sockets opened and 1468 reused (97.9% reuse events among measured opens and reuses).
- Concurrency trial: 1000 requests submitted with 1000 clients; 550.0 requests/s; peak active gateway workers 718; peak gateway RSS 29.7 MiB.
- Packet capture: unavailable: tcpdump lacks permission to capture on `lo` in this environment.
- Supplemental reuse syscall trace: `bench/results/phase5_reuse_syscalls.txt` records one upstream `connect()` and two request writes on the same descriptor in the two-request reuse test.
(socket: Operation not permitted).

## Acceptance against handbook targets

- Gateway overhead p95 <= 5 ms: PASS at concurrency 32; MISS under the 1,000-client trial.
- Throughput >= 5,000 requests/s: MISS; local Python proxy throughput is the measured bottleneck.
- 1,000 in-flight requests: MISS; observed peak was 718.
- Memory <= 256 MiB at 1,000 clients: PASS; measured peak gateway RSS was 29.7 MiB.
- Keep-alive lower latency than connection-per-request: PASS in the gateway comparison.

The 1,000-client trial completed its requests, but the measured peak active workers did not reach 1,000. RSS is the gateway process's `/proc/<pid>/status` VmRSS peak. The overhead metric subtracts forwarder-attempt duration from `GatewayCore::handle` and excludes queueing before the core and client response writes. The proxy throughput figure identifies the Python fault proxy as a material bottleneck.
