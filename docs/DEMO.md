# Phase 6 demo

## Start and open the dashboard

From the repository root:

```bash
cmake -S . -B build
cmake --build build -j
./scripts/demo.sh start
```

Open [http://127.0.0.1:9000/ui](http://127.0.0.1:9000/ui). The gateway uses `config/gateway.demo.json`, selects the Phase 5 epoll forwarder, and appends redacted request rows to `/tmp/phase6-demo-requests.csv`.

This is a reproducible local test-bed demo: the three proxies forward to the local target and expose bounded fault injection. It does not use third-party proxy providers or provider credentials. The dashboard shows configured proxy endpoints read-only; use the scenario settings to change delay and injected failure rates.

Click **Start load generator**. It sends requests to `site-a.example` and `site-b.example`; the former has a 50 requests/s limit and the latter uses the default limit. Watch request rate, success rate, p50/p95/p99, AIMD concurrency, queues, proxy status and recent requests.

## Demonstrate controls

1. Click **Start load generator**. It sends traffic to both configured domains.
2. Under **Proxy scenario settings**, use a preset or edit each proxy's delay and failure rate, then click **Apply** on that row. The changes take effect immediately; gateway health, circuits and charts continue updating live.
3. Choose **Combined delay and failures** to add 100 ms to Proxy 2 and a 20% failure rate to Proxy 3, or use **Healthy baseline** to clear all injected faults.
4. Choose **Weighted Score** in Routing policy, enter a rate and burst, leave **Health-aware routing** enabled, and click **Apply settings**. The policy and default-domain bucket apply to subsequent jobs; `site-a.example` keeps its explicit 50/s override.
5. Click **Kill Proxy 3**. Wait for its circuit to open and observe traffic move to eligible proxies. Click **Restore test bed** to resume it and clear injected faults; Phase 4 probing moves its circuit through HALF_OPEN to CLOSED when healthy.
6. Click **Stop load generator** before running benchmarks.
7. Click **Run benchmark**. The comparison table is also saved as `bench/results/phase6_benchmark.csv`; interpretation and the health-aware fault-trial scope are in the matching `.md` report.

## Recovery and cleanup

- If Proxy 3 is stopped, click **Restore test bed**. In local mode this resumes the test-bed process; in Docker mode it starts the fixed `proxy3` container.
- If the dashboard reports its control service unavailable, restart the local services with `./testbed/run_local.sh start`.
- Stop everything with `./scripts/demo.sh stop`.

## Rehearsal record

UNVERIFIED: two clean-start rehearsals are required before Phase 6 sign-off. Record dates, observed recovery, and any deviations here after rehearsing on the target host. Unit/sanitizer suites and runtime walkthroughs are also outstanding; see `docs/HANDOFF_PHASE_6.md`.
