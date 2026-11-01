# Phase 3 routing policy comparison

Run on 2026-11-01 with `python3 bench/phase3_compare.py --requests 100 --concurrency 8 --output bench/results/phase3_routing.csv` using the local Python test bed, GCC 13.3, and the `FAIL_SEED=43031` Proxy 3 fault pattern. Each policy used 100 requests and 8 concurrent client connections; Proxy 2 adds 100 ms and Proxy 3 injects 20% HTTP 502 responses. These are one-run demonstration numbers, not stable performance claims.

| Policy | p50 ms | p95 ms | p99 ms | Throughput req/s | Success rate |
| --- | ---: | ---: | ---: | ---: | ---: |
| random | 82.58 | 189.55 | 194.64 | 101.86 | 0.920 |
| roundrobin | 91.50 | 192.53 | 195.98 | 106.01 | 0.900 |
| leastconn | 5.43 | 39.59 | 103.61 | 482.21 | 0.900 |
| latency | 6.10 | 103.41 | 104.69 | 564.26 | 0.850 |
| weighted | 6.04 | 103.85 | 105.61 | 549.07 | 0.900 |

Least-connections had the highest throughput and lowest p50 in this run. Latency and weighted were close behind on throughput and p50. Random and round-robin sent more work through the configured slow proxy under this sample and had much higher tail latency. Success rates vary from 0.85 to 0.92 because the sample is small and the test bed injects failures; they do not establish a meaningful reliability ranking. Repeat with larger samples before drawing conclusions about policy quality.
