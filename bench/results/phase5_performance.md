# Phase 5 performance results

Run date: 2026-11-03 19:46:59 IST. Local test bed: Python target and fault proxies on loopback; gateway compiled with the current workspace build.

## Measurements

- Direct target throughput: 4427.2 requests/s (500 requests, concurrency 32).
- Direct proxy throughput (new client connection per request): 2196.3 requests/s.
- Direct proxy keep-alive: p50 0.39 ms, p95 0.57 ms.
- Gateway simple: sequential p95 latency 1.48 ms (median of 3 x 1,000-request runs); throughput 1291.5 requests/s at concurrency 32.
- Gateway epoll: sequential p95 latency 1.26 ms (median of 3 x 1,000-request runs); throughput 1684.0 requests/s at concurrency 32.
- Epoll GatewayCore overhead: p95 0.08 ms at concurrency 32; p95 3.29 ms during the 1,000-request trial.
- Epoll reuse: 32 sockets opened and 3468 reused (99.1% reuse events among measured opens and reuses).
- Concurrency trial: 1000 requests submitted with 1000 clients; 320.6 requests/s; peak active gateway workers 1000; peak gateway RSS 31.4 MiB.
- Packet capture: unavailable: tcpdump on `lo` returned `Operation not permitted`; this runtime lacks capture capability.
- Supplemental reuse syscall trace: `bench/results/phase5_reuse_syscalls.txt` records one upstream `connect()` and two request writes on the same descriptor in the two-request reuse test.

## Acceptance against handbook targets

- Gateway overhead p95 <= 5 ms: PASS at concurrency 32; PASS under the 1,000-client trial.
- Throughput >= 5,000 requests/s: MISS; local Python proxy throughput is the measured bottleneck.
- 1,000 in-flight requests: PASS; observed peak was 1000.
- Memory <= 256 MiB at 1,000 clients: PASS; measured peak gateway RSS was 31.4 MiB.
- Keep-alive lower latency than connection-per-request: PASS in the gateway comparison.

The 1,000-client trial completed its requests and observed a peak of 1000 active gateway workers. The controlled target delay is 2,000 ms to keep requests in flight while the client batch starts. RSS is the gateway process's `/proc/<pid>/status` VmRSS peak. The overhead metric subtracts forwarder-attempt duration from `GatewayCore::handle` and excludes ingress queueing and client response writes. The Python target and fault proxy cap the local throughput baseline; the recorded direct measurements show their current ceiling.
