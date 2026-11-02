# Phase 6 benchmark comparison

Run date: 2026-11-03 00:09:32 IST

Each scenario sends 120 requests at concurrency 24 through the current gateway build. Percentiles use client-observed request duration. Metrics accuracy compares completed client responses with the gateway's `requests_total` delta.

| Scenario | Setting | req/s | p50 ms | p95 ms | p99 ms | Success | Counter delta | Interpretation |
|---|---|---:|---:|---:|---:|---:|---:|---|
| routing_policy | random | 227.15 | 3.873 | 300.014 | 301.857 | 100.0% | 0.0% | difference under 5%; inconclusive at this sample size |
| routing_policy | roundrobin | 232.15 | 4.081 | 301.092 | 304.554 | 100.0% | 0.0% | reference measurement |
| routing_policy | leastconn | 908.62 | 6.674 | 102.021 | 103.148 | 100.0% | 0.0% | 291.4% higher throughput than roundrobin |
| routing_policy | latency | 386.14 | 3.772 | 302.707 | 304.407 | 100.0% | 0.0% | 66.3% higher throughput than roundrobin |
| routing_policy | weighted | 385.75 | 3.152 | 299.422 | 300.477 | 100.0% | 0.0% | 66.2% higher throughput than roundrobin |
| pool_size | 2 | 58.96 | 3.38 | 1217.154 | 1217.858 | 100.0% | 0.0% | 74.7% lower throughput than 8 |
| pool_size | 4 | 117.24 | 3.475 | 610.041 | 611.596 | 100.0% | 0.0% | 49.6% lower throughput than 8 |
| pool_size | 8 | 232.75 | 3.432 | 300.35 | 302.38 | 100.0% | 0.0% | reference measurement |
| pool_size | 16 | 378.74 | 4.396 | 190.033 | 192.149 | 100.0% | 0.0% | 62.7% higher throughput than 8 |
| worker_mode | fixed | 229.35 | 4.478 | 300.896 | 303.015 | 100.0% | 0.0% | reference measurement |
| worker_mode | aimd | 115.09 | 204.536 | 305.995 | 307.591 | 100.0% | 0.0% | 49.8% lower throughput than fixed |
| keep_alive | off_simple | 523.23 | 4.242 | 103.98 | 105.496 | 100.0% | 0.0% | reference measurement |
| keep_alive | on_epoll | 232.96 | 3.663 | 302.61 | 304.602 | 100.0% | 0.0% | 55.5% lower throughput than off_simple |
| health_aware_fault_trial | faults_off | 385.75 | 3.085 | 303.084 | 305.019 | 100.0% | 0.0% | difference under 5%; inconclusive at this sample size |
| health_aware_fault_trial | faults_on_aware | 385.3 | 3.87 | 302.931 | 304.509 | 99.2% | 0.0% | reference measurement |
| health_aware_fault_trial | faults_on_unaware | 385.76 | 3.605 | 302.889 | 304.606 | 95.8% | 0.0% | difference under 5%; inconclusive at this sample size |

The `health_aware_fault_trial` compares the same injected Proxy 3 fault with health-aware selection enabled and disabled. The disabled mode ignores the rolling healthy flag but still excludes OPEN and HALF_OPEN circuits. Treat differences under 5% as inconclusive. This small local test-bed sample is comparative, not a claim that the handbook's absolute performance targets are met.
