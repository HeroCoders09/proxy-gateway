# Phase 6 benchmark comparison

Run date: 2026-11-03 18:59:19 IST

Each scenario sends 120 requests at concurrency 24 through the current gateway build. Percentiles use client-observed request duration. Metrics accuracy compares completed client responses with the gateway's `requests_total` delta.

| Scenario | Setting | req/s | p50 ms | p95 ms | p99 ms | Success | Counter delta | Interpretation |
|---|---|---:|---:|---:|---:|---:|---:|---|
| routing_policy | random | 1405.16 | 11.459 | 20.453 | 23.801 | 100.0% | 0.0% | 5.5% lower throughput than roundrobin |
| routing_policy | roundrobin | 1486.26 | 12.107 | 19.09 | 22.491 | 100.0% | 0.0% | reference measurement |
| routing_policy | leastconn | 1464.19 | 11.721 | 20.214 | 25.004 | 100.0% | 0.0% | difference under 5%; inconclusive at this sample size |
| routing_policy | latency | 1407.16 | 12.094 | 31.332 | 36.883 | 100.0% | 0.0% | 5.3% lower throughput than roundrobin |
| routing_policy | weighted | 1551.22 | 12.924 | 18.549 | 22.176 | 100.0% | 0.0% | difference under 5%; inconclusive at this sample size |
| pool_size | 2 | 1100.34 | 14.673 | 29.629 | 32.847 | 100.0% | 0.0% | 6.5% lower throughput than 8 |
| pool_size | 4 | 1453.0 | 12.617 | 21.391 | 24.343 | 100.0% | 0.0% | 23.5% higher throughput than 8 |
| pool_size | 8 | 1176.74 | 17.187 | 28.616 | 33.166 | 100.0% | 0.0% | reference measurement |
| pool_size | 16 | 1080.05 | 15.206 | 33.146 | 38.118 | 100.0% | 0.0% | 8.2% lower throughput than 8 |
| worker_mode | fixed | 1190.17 | 13.817 | 27.227 | 33.322 | 100.0% | 0.0% | reference measurement |
| worker_mode | aimd | 1486.4 | 10.867 | 20.933 | 24.813 | 100.0% | 0.0% | 24.9% higher throughput than fixed |
| keep_alive | off_simple | 1501.91 | 12.071 | 19.193 | 22.978 | 100.0% | 0.0% | reference measurement |
| keep_alive | on_epoll | 1551.61 | 10.746 | 19.65 | 24.135 | 100.0% | 0.0% | difference under 5%; inconclusive at this sample size |
| health_aware_fault_trial | faults_off | 1431.52 | 13.3 | 20.065 | 22.893 | 100.0% | 0.0% | 5.4% lower throughput than faults_on_aware |
| health_aware_fault_trial | faults_on_aware | 1513.35 | 9.231 | 27.179 | 29.772 | 94.2% | 0.0% | reference measurement |
| health_aware_fault_trial | faults_on_unaware | 1683.1 | 9.907 | 17.768 | 21.167 | 95.8% | 0.0% | 11.2% higher throughput than faults_on_aware |

The `health_aware_fault_trial` compares the same injected Proxy 3 fault with health-aware selection enabled and disabled. The disabled mode ignores the rolling healthy flag but still excludes OPEN and HALF_OPEN circuits. Treat differences under 5% as inconclusive. This small local test-bed sample is comparative, not a claim that the handbook's absolute performance targets are met.
