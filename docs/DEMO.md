# Phase 6 demo

## Start and open the dashboard

From the repository root:

```bash
cmake -S . -B build
cmake --build build -j
./scripts/demo.sh start
```

Open [http://127.0.0.1:9000/ui](http://127.0.0.1:9000/ui). The gateway uses `config/gateway.demo.json`, selects the Phase 5 epoll forwarder, and appends redacted request rows to `/tmp/phase6-demo-requests.csv`.

This demo requires Basic proxy authentication and an exact hostname/port allowlist for its three fictional retailer hosts. If `PROXY_GATEWAY_CLIENT_PASSWORD` is not provided, `scripts/demo.sh start` generates a local password (mode 0600) in the ignored `testbed/.run/client-password` file. It passes the password to the gateway and offer monitor without printing it.

To make one authorized client request through the gateway:

```bash
curl -x http://127.0.0.1:8000 \
  --proxy-user "pricelens:$(cat testbed/.run/client-password)" \
  http://northstar.pricelens.test/api/offers/SKU-1042
```

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

| Date | Start and checks | Recovery / cleanup |
| --- | --- | --- |
| 2026-11-03 · local rehearsal 1 | Clean `./scripts/demo.sh start`; dashboard and live APIs loaded; exercised policy/rate controls, latency ladder, offer monitor, kill/circuit opening and restore; observed synthetic offers and recent requests. | Stopped the offer monitor, restored baseline, then ran `./scripts/demo.sh stop`; all processes stopped. |
| 2026-11-03 · local rehearsal 2 | Removed the first run's generated password and restarted from stopped services; checked dashboard/live stats and authenticated, blocked, and unauthenticated requests (200/403/407). | `./scripts/demo.sh stop`; status confirmed no local services running. |

Phase 6 runtime acceptance, normal and ASan/UBSan tests, benchmark counter accuracy, and local plus Docker proxy controls are recorded in `docs/HANDOFF_PHASE_6.md`. Editable SRS/Design Document source files were not available in the repository for direct updates; see D-039.

## Optional real-proxy demonstration

The gateway accepts proxy host and port and sends ordinary HTTP forward-proxy requests. A provider requiring HTTP Basic authentication can be configured in a separate local config using `proxies[].auth.username` and `proxies[].auth.password_env`; export the referenced secret (minimum 16 characters) before starting the gateway. Never put provider credentials in tracked config or docs. The provider connection itself is plain HTTP, so use this only on a trusted/private network or with a provider-side network restriction. Client HTTPS CONNECT, dashboard endpoint editing, and provider lifecycle controls are not implemented. The Phase 6 dashboard fault controls apply only to the bundled test bed.

## Optional TLS-protected Docker demo

For a TLS connection from the proxy client to the gateway, use the Docker Compose profile:

```bash
scripts/docker-demo-tls.sh start
```

If `PROXY_GATEWAY_CLIENT_PASSWORD` is unset, the script generates a local password and stores it in ignored `testbed/.run/`. It creates a self-signed certificate for `localhost` when trusted certificate files are absent; certificate and key files stay in ignored `testbed/tls/certs/`. The self-signed certificate is for local demonstration only. The TLS listener binds to `127.0.0.1:8443` by default. The curl example below assumes the script generated the password file.

```bash
curl --proxy https://localhost:8443 \
  --proxy-cacert testbed/tls/certs/fullchain.pem \
  --proxy-user "pricelens:$(cat testbed/.run/docker-client-password)" \
  http://northstar.pricelens.test/api/offers/SKU-1042
scripts/docker-demo-tls.sh stop
```

For deployment, set `TLS_CERT_DIR` to a directory containing a trusted `fullchain.pem` and `privkey.pem`, set a strong `PROXY_GATEWAY_CLIENT_PASSWORD`, and select an appropriate `TLS_BIND`. The Compose gateway's port 8000 remains published only on host loopback; admin stays on host loopback. The TLS front end protects client-to-gateway transport only. It does not add HTTPS `CONNECT` support for destinations.
