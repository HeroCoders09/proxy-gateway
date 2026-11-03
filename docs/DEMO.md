# Phase 6 demo

## Start and open the dashboard

From the repository root:

```bash
cmake -S . -B build
cmake --build build -j
./scripts/demo.sh start
```

Open [http://127.0.0.1:9000/ui](http://127.0.0.1:9000/ui). The gateway uses `config/gateway.demo.json`, selects the Phase 5 epoll forwarder, and appends redacted request rows to `/tmp/phase6-demo-requests.csv`.

The story is **PriceLens**, a service that checks the same SKU across Northstar, Harbor Market, and Copper Lane, then routes those HTTP checks through a proxy pool. These retailer names resolve only inside the test bed. Their deterministic sample prices and stock statuses are synthetic; this demo does not scrape real shops or use third-party proxy providers. Five local proxies expose bounded fault injection and different starting profiles. The dashboard shows configured endpoints read-only; use scenario settings to change delay and injected failure rates.

Click **Start offer monitor**. It sends repeated requests for five SKUs to all three retailer hosts through the gateway. The **Live market watch** table fills with the latest returned offer per SKU and retailer. Northstar has a 50 requests/s limit; the other two use the default limit. Watch check rate, success rate, p50/p95/p99, AIMD concurrency, proxy status, and retailer request hosts.

The five local proxy defaults are:

| Proxy | Endpoint | Starting behavior |
| --- | --- | --- |
| Proxy 1 | `127.0.0.1:3128` | Baseline |
| Proxy 2 | `127.0.0.1:3129` | 100 ms extra delay |
| Proxy 3 | `127.0.0.1:3130` | 20% injected failures |
| Proxy 4 | `127.0.0.1:3131` | 250 ms extra delay |
| Proxy 5 | `127.0.0.1:3132` | 40% injected failures |

The dashboard can edit delay/failure values on every row. Fault-control ports are 3228–3232.

## Demonstrate controls

1. Click **Start offer monitor** and wait for the first rows in Live market watch.
2. Under **Proxy scenario settings**, use a preset or edit each proxy's delay and failure rate, then click **Apply** on that row. The changes take effect immediately; gateway health, circuits and charts continue updating live.
3. Choose **Latency ladder** to apply 80, 160, 250, and 400 ms delay to Proxies 2–5; choose **Reliability gradient** to apply 5%, 15%, 25%, and 40% failure rates; or choose **Mixed latency and failures** to combine delays and failures. **Healthy baseline** clears injected faults on all five proxies.
4. Choose **Weighted Score** in Routing policy, enter a rate and burst, leave **Health-aware routing** enabled, and click **Apply settings**. The policy and default-domain bucket apply to subsequent jobs; Northstar keeps its explicit 50/s override.
5. Click **Kill Proxy 3**. Wait for its circuit to open and observe traffic move to eligible proxies. Click **Restore baseline** to resume it and clear injected faults on all five proxies; Phase 4 probing moves its circuit through HALF_OPEN to CLOSED when healthy.
6. Click **Stop offer monitor** before running benchmarks.
7. Click **Run benchmark**. The comparison table is also saved as `bench/results/phase6_benchmark.csv`; interpretation and the health-aware fault-trial scope are in the matching `.md` report.

## Recovery and cleanup

- If Proxy 3 is stopped, click **Restore baseline**. In local mode this resumes the test-bed process; in Docker mode it starts the fixed `proxy3` container.
- If the dashboard reports its control service unavailable, restart the local services with `./testbed/run_local.sh start`.
- Stop everything with `./scripts/demo.sh stop`.

## Rehearsal record

UNVERIFIED: two clean-start rehearsals are required before Phase 6 sign-off. Record dates, observed recovery, and any deviations here after rehearsing on the target host. Unit/sanitizer suites and runtime walkthroughs are also outstanding; see `docs/HANDOFF_PHASE_6.md`.

## Optional real-proxy demonstration

The gateway's current proxy configuration accepts only a proxy host and port and sends ordinary HTTP forward-proxy requests. A provider endpoint could be configured in a separate local config and used for an authorized HTTP test, if the provider supports unauthenticated or IP-allow-listed HTTP proxy access. Do not put provider credentials in the repository. Username/password proxy authentication, HTTPS CONNECT from clients, dashboard endpoint editing, and provider lifecycle controls are not implemented. The Phase 6 dashboard fault controls apply only to the bundled test bed.
