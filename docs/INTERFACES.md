# Interfaces

Source of truth for shared structs, module interfaces, config keys and the admin API. Changing anything here needs a decision entry in `docs/DECISIONS.md` and an entry in the change history at the bottom, in the same commit. Fields may be added; they may not be renamed or removed without a decision.

Code locations: structs in `src/common/types.hpp`, interfaces in `src/common/interfaces.hpp`, metrics in `src/common/metrics.hpp`, config in `src/common/config.hpp`, clock in `src/common/clock.hpp`, descriptor wrapper in `src/common/fd.hpp`. All in namespace `pg`.

## Structs

| Struct | Fields (type) | Notes |
| --- | --- | --- |
| `Job` | `id` (u64), `url` (string, as received), `domain` (string, lower-case host without port: the queue and rate-limit key), `method`, `headers` (vector of `{name,value}`), `body`, `enqueue_time` (steady `TimePoint`), `retry_count` (int), `tried_proxies` (vector of `ProxyId`), `client_fd` (int, NOT owned), plus parsed `target_host`, `target_port` (u16, default 80), `target_path` (path and query, starts with `/`) | `client_fd` stays valid only while the ingress owns the connection; Phase 2 must keep the connection alive until the reply is written |
| `ProxyState` | `id` (`ProxyId` = int, index in config `proxies`), `host`, `port`, `healthy`, `rtt_ms` (EMA), `rolling_rtt_ms`, `rolling_success_rate`, traffic `success_count` / `failure_count`, probe `probe_success_count` / `probe_failure_count`, `consecutive_failures`, `active_connections`, `last_probe_time`, `last_probe_unix_ms`, `circuit_open_count`, `circuit_state` (`Closed`, `Open`, `HalfOpen`; JSON strings `CLOSED`, `OPEN`, `HALF_OPEN`) | Phase 3 builds the thread-safe table; Phase 4 drives health and circuit fields from combined rolling outcomes |
| `DomainQueue` | `jobs` (deque of `Job`), `queue_limit` (default 10000), `tokens`, `refill_rate` (tokens per second), `capacity` (burst B), `last_refill` | Phase 2 |
| `Connection` | `fd` (`Fd`), `proxy_id`, `state` (`ConnState`: `Idle`, `Connecting`, `Active`, `Closed`), `last_used`, `requests_served`, `keep_alive`, `in_buf`, `out_buf` | Phase 5 |
| `Metrics` (implements `MetricsSink`) | counters `RequestsTotal`, `RequestsSuccessful`, `RequestsFailed`, `Timeouts`, `BadRequests`, `QueueRejected`, `Retries`, `NoRoute`, and policy selections (`RandomSelections`, `RoundRobinSelections`, `LeastConnSelections`, `LatencySelections`, `WeightedSelections`); bounded latency ring (4096 samples) giving p50, p95, p99 | Retries counts additional attempts, NoRoute counts requests with no eligible proxy |
| `GatewayConfig` | see "Config file keys" | Loaded and validated by `load_config_file` |
| `HttpResponse` (`src/ingress/http_response.hpp`) | `status` (int), `headers`, `body` | Used for every client reply; `write_response(fd, resp, head_only, timeout_ms)` sends it |
| `ForwardResult` | `error` (`ErrorKind`), `error_detail` (short string, never a body or credential), `status`, `headers`, `body`, `latency_ms` (connect + request + response), `proxy_id`; `ok()` is true when `error == None` | |

`ErrorKind`: `None`, `ConnectTimeout`, `ConnectFailed`, `ResponseTimeout`, `IoError`, `BadResponse`. Strings (`to_string`): `none`, `connect_timeout`, `connect_failed`, `response_timeout`, `io_error`, `bad_response`. `is_timeout(kind)` is true for the two timeout kinds. Phase 3 retry logic keys off this.

## Module interfaces

| Interface | Signature and contract |
| --- | --- |
| `Forwarder` | `ForwardResult forward(const Job& job, const ProxyState& proxy)`. Thread-safe: may be called from many threads. Never throws for network problems; they come back as `ForwardResult.error`. Implemented by `SimpleForwarder` (Phase 1, `src/net/simple_forwarder.*`); Phase 5's epoll forwarder implements the same interface and is chosen by config `forwarder` |
| `Router` | `std::optional<ProxyId> select(const Job& job, const std::vector<ProxyState>& table)`. Returns `nullopt` when no route is available. `PolicyRouter` implements all five policies; candidates must be healthy, CLOSED, and not in `job.tried_proxies`. OPEN and HALF_OPEN are excluded; HALF_OPEN is reserved for its single monitor probe (D-024). Latency mode explores eligible proxies without a sample before choosing minimum measured RTT (D-022) |
| `RateLimiter` | `bool try_acquire(const std::string& domain)`. Must use a `Clock` that tests can fake. Phase 2 |
| `HealthReporter` | `void report(ProxyId proxy_id, bool success, double rtt_ms)`. `GatewayCore` implements it: each real attempt updates traffic counts, rolling health and RTT, consecutive failures and circuit state. Probe reports update rolling health and separate probe counts |
| `MetricsSink` | `void inc(Counter c, uint64_t n = 1)`, `void add_gauge(Gauge g, int64_t delta)`, `void observe_latency_ms(double ms)` | Thread-safe; the only way modules write measurements. New counters and gauges append before their respective `kCount` values; never reorder |
| `Clock` | `TimePoint now() const` on `std::chrono::steady_clock`. `SteadyClock` for production, `FakeClock` (`advance(d)`) for tests |
| `Fd` | RAII file-descriptor owner: `get()`, `valid()`, `release()`, `reset(fd)`, move-only. No raw `close()` anywhere else |
| `Listener` | Legacy `Handler = std::function<HttpResponse(Job&)>` remains for tests. Phase 2 production uses `AsyncHandler = std::function<void(Job, Fd, bool head_only)>`, transferring the descriptor to the scheduler; `start()`, `stop()`, `port()` |
| `DomainScheduler` | `enqueue(Job, Fd&, head_only) -> bool`, `try_acquire(domain) -> bool`, `queue_lengths()`. Owns one FIFO/token bucket per domain; false means the caller sends 503. |
| `WorkerPool` | `submit(Job, Fd, head_only)`, fixed `workers.max` threads, `active_jobs()`, `aimd_limit()`. It writes replies after forwarding; no worker is created per request. |

Request path in Phase 1: `Listener` parses to `Job` -> `GatewayCore::handle(Job&)` -> `SimpleForwarder::forward(job, proxy0)` -> `HttpResponse` -> client.

## Config file keys

Strict: unknown keys, wrong types and out-of-range values are rejected (exit code 2). Defaults apply when a key is absent. Example: `config/gateway.json`. Keys marked "later" are validated now but used by a later phase.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `listen.host` | IPv4 string | `0.0.0.0` | Client port bind address |
| `listen.port` | int 1-65535 | 8000 | Client port |
| `admin.host` | IPv4 string | `127.0.0.1` | Admin bind address (localhost by default, SRS security) |
| `admin.port` | int 1-65535 | 9000 | Admin and dashboard port |
| `proxies` | array of `{host: string, port: int}` | required, at least 1 | Proxy pool; `ProxyId` is the array index. Phase 1 uses element 0 only |
| `policy` | `random`, `roundrobin`, `leastconn`, `latency`, `weighted` | `roundrobin` | Active routing policy. Overridden by `--policy` |
| `forwarder` | `simple`, `epoll` | `simple` | Forwarder implementation. `epoll` selects the non-blocking reactor with a bounded keep-alive pool (D-030) |
| `forwarder_pool.max_connections_per_proxy` | int 1-4096 | 8 | Max open upstream sockets for one proxy (epoll only) |
| `forwarder_pool.max_total_connections` | int 1-16384 | 128 | Max open upstream sockets across the gateway; must be at least the per-proxy limit |
| `forwarder_pool.idle_timeout_ms` | int 1-3,600,000 | 30000 | Idle keep-alive socket eviction timeout |
| `rate_limits.default.rate` | number > 0 | 1000 | Token refill per second for domains with no entry (later: Phase 2) |
| `rate_limits.default.burst` | number > 0 | 2000 | Bucket capacity B |
| `rate_limits.<domain>.rate` / `.burst` | number > 0 | inherit default | Per-domain override; domain names are lower-cased |
| `queue_limit` | int 1-10,000,000 | 10000 | Per-domain queue limit; overflow gives 503 (later: Phase 2) |
| `workers.min` / `workers.max` | int 1-100000 | 4 / 64 | AIMD bounds; `max` must be at least `min` (later: Phase 2). `--workers N` overrides `max` |
| `workers.alpha` | number > 0 | 1 | Additive increase |
| `workers.beta` | number, 0 < beta < 1 | 0.5 | Multiplicative decrease |
| `timeouts.connect_ms` | int 1-3,600,000 | 2000 | Connect timeout per attempt |
| `timeouts.response_ms` | int 1-3,600,000 | 10000 | Response timeout per attempt, measured from connect completion |
| `circuit.threshold` | int >= 1 | 5 | Consecutive failures that open a circuit |
| `circuit.cooldown_ms` | int 1-3,600,000 | 10000 | OPEN to HALF_OPEN cooldown |
| `circuit.probe_interval_ms` | int 1-3,600,000 | 2000 | Per-proxy health probe cadence |
| `circuit.window_size` | int 1-1,000,000 | 20 | Number of recent real attempts and probes used for rolling health and RTT statistics |
| `weights.latency` / `.load` / `.failure` | number >= 0 | 0.5 / 0.3 / 0.2 | Weighted-score components; normalized per eligible set (D-020) |
| `ingress.threads` | int 1-4096 | 16 | Fixed handler-thread pool |
| `ingress.max_header_bytes` | int 256 to 1 MiB | 16384 | Larger request heads get 400 |
| `ingress.max_body_bytes` | int >= 1 | 1048576 | Larger bodies get 400 |
| `ingress.read_timeout_ms` | int > 0 | 5000 | Time a client has to deliver a full request |
| `ingress.pending_connections` | int >= 1 | 1024 | Bound on accepted-but-unserved connections; overflow gets 503 |
| `output.format` | `none`, `csv`, `json` | `none` | Result writer (Phase 6) |
| `output.path` | string | empty | Required when `format` is not `none` |

CLI: `gateway --config FILE [--policy NAME] [--workers N] [--help]`. `--config` is required. Exit code 0 on `--help`, 2 for any configuration or CLI error (message `gateway: error: ...` on stderr), 1 if a port cannot be bound.

Ready-made configs: `config/gateway.json` (documented three-proxy example), `config/gateway.local.json` (host gateway with the original three local proxies), `config/gateway.demo.json` (five-proxy PriceLens demo), and `config/gateway.docker.json` (five proxies for the optional gateway container; admin binds inside the container).

## Gateway-generated responses

| Status | When |
| --- | --- |
| 400 | Malformed, oversized or incomplete request (see D-007) |
| 502 | Connect failure after retries, I/O error or unparseable proxy response, handler exception, no eligible route |
| 503 | Ingress pending queue full (later: domain queue full) |
| 504 | Connect or response timeout |
| 429 | Reserved |

All are `text/plain` with a short fixed message. Proxied responses keep the target's status and gain `X-Gateway-Proxy: <proxy id>`. Phase 3 retries `ConnectTimeout`, `ResponseTimeout`, `ConnectFailed`, `IoError` and `BadResponse` transport results through a different proxy, with at most two retries. HTTP responses, including 5xx, pass through without retry.

## Admin API

Served on `admin.host:admin.port` (default `127.0.0.1:9000`). Read endpoints return JSON unless noted. Phase 1 served dummy data (`"stub": true`); Phase 6 uses live snapshots (`"stub": false`). Dashboard code must ignore unknown fields.

Units: times in milliseconds unless the name says otherwise; `_unix_ms` is milliseconds since the Unix epoch; rates are per second; `success_rate` is a fraction from 0 to 1.

### `GET /health`

```json
{"status": "ok", "uptime_seconds": 12.5, "version": "0.1.0", "phase": 6, "stub": false}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `status` | string | `"ok"` while the process is serving |
| `uptime_seconds` | number | Seconds since start |
| `version` | string | Gateway version |
| `phase` | int | Latest completed phase (informational) |
| `stub` | bool | false when values are live |

### `GET /proxies`

```json
{"stub": false, "proxies": [
  {"id": 0, "host": "127.0.0.1", "port": 3128, "healthy": true, "rtt_ms": 0.0,
   "requests_sent": 0, "success_count": 0, "failure_count": 0, "consecutive_failures": 0, "success_rate": 1.0,
   "active_connections": 0, "circuit_state": "CLOSED", "circuit_open_count": 0, "last_probe_unix_ms": 0,
   "rolling_rtt_ms": 0.0, "probe_success_count": 0, "probe_failure_count": 0}
]}
```

| Field | Type | Meaning | Filled by |
| --- | --- | --- | --- |
| `id` | int | Index in config `proxies` | Phase 1 |
| `host`, `port` | string, int | Proxy address | Phase 1 |
| `healthy` | bool | Eligibility flag; derived from the latest four combined outcomes (or full smaller window), unhealthy below 50% (D-027) | Phase 4 |
| `rtt_ms` | number | Recent smoothed RTT (EMA alpha 0.2) | Phase 3 |
| `success_count`, `failure_count` | int | Attempts through this proxy; HTTP 5xx counts as failure | Phase 3 |
| `requests_sent` | int | Total attempts through this proxy (`success_count + failure_count`) | Phase 3 |
| `consecutive_failures` | int | Current attempt failure streak | Phase 3 |
| `success_rate` | number 0..1 | Combined rolling request-and-probe success rate, last `circuit.window_size` samples | Phase 4 |
| `active_connections` | int | In-flight requests through this proxy; reservation released after every completion path | Phase 3 |
| `circuit_state` | string | `CLOSED`, `OPEN` or `HALF_OPEN` | Phase 4 |
| `circuit_open_count` | int | Times the circuit has opened | Phase 4 |
| `last_probe_unix_ms` | int | Time of the last health probe, 0 if none | Phase 4 |
| `rolling_rtt_ms` | number | Mean of nonzero RTTs in the rolling window | Phase 4 |
| `probe_success_count`, `probe_failure_count` | int | Probe outcomes, kept separate from real request attempt totals | Phase 4 |

### `GET /api/stats` (dashboard stats endpoint)

```json
{"stub": false, "timestamp_unix_ms": 1790000000000, "policy": "roundrobin", "health_aware": true,
 "requests_total": 0, "requests_successful": 0, "requests_failed": 0, "timeouts": 0,
 "health_probe_successes": 0, "health_probe_failures": 0,
 "retries": 0, "no_route": 0, "selections": {"random": 0, "roundrobin": 0, "leastconn": 0, "latency": 0, "weighted": 0},
 "requests_per_second": 0.0, "success_rate": 1.0, "active_connections": 0, "worker_count": 0,
 "upstream_connections_opened": 0, "upstream_connections_reused": 0,
 "upstream_connect_timeouts": 0, "upstream_response_timeouts": 0,
 "upstream_bytes_in": 0, "upstream_bytes_out": 0,
 "upstream_active_connections": 0, "upstream_idle_connections": 0,
 "gateway_overhead_ms": {"p50": 0.0, "p95": 0.0, "p99": 0.0},
 "aimd_limit": 64, "queue_length": {}, "output_rows_dropped": 0,
 "latency_ms": {"p50": 0.0, "p95": 0.0, "p99": 0.0}}
```

| Field | Type | Meaning | Real from |
| --- | --- | --- | --- |
| `timestamp_unix_ms` | int | Server time of the snapshot | Phase 1 |
| `policy` | string | Active routing policy | Phase 1 (value from config) |
| `requests_total`, `requests_successful`, `requests_failed`, `timeouts` | int | Counters (definitions in D-010) | Phase 1 |
| `health_probe_successes`, `health_probe_failures` | int | Health probe outcome counters; excluded from request totals | Phase 4 |
| `retries`, `no_route` | int | Additional forwarded attempts and requests unable to select an eligible route | Phase 3 |
| `selections` | object of policy name to int | Number of proxy selections made under each policy | Phase 3 |
| `requests_per_second` | number | Rate over the last few seconds | Phase 6 |

### `GET /api/recent`

Returns up to the 50 most recently completed worker requests as `{"requests":[...]}`. Each row contains `timestamp_unix_ms`, a redacted `url` (authority credentials, path and query are omitted), `status`, `proxy` (zero-based ID or -1), `latency_ms` and `success`.

### `GET /api/control/settings?policy=...&rate=...&burst=...&health_aware=true|false`

Local dashboard control endpoint. Accepts one of the five documented routing policies and bounded rate/burst values; applies these and the optional rolling-health eligibility toggle at runtime. OPEN and HALF_OPEN circuits remain ineligible in either mode. The endpoint is available only through the admin listener, which is localhost-bound by default.

### `GET /ui`

Serves the static dashboard from `dashboard/index.html`. The page polls live stats, proxy, health and recent-request endpoints once per second. Fault controls and benchmark launch use the separate localhost-only test-bed control service on port 9100.

### Phase 6 test-bed control service (`127.0.0.1:9100`)

This separate service controls only the bundled local/Docker test bed. Mutations use `POST`; reads use `GET`. Browser origins, when present, are limited to the local dashboard origins.

| Route | Behavior |
| --- | --- |
| `GET /proxy-settings` | Current delay/failure settings and availability for fixed test proxies `proxy1` through `proxy5` |
| `POST /proxy-settings?proxy=proxyN&delay_ms=N&fail_pct=P` | Set delay 0–5,000 ms and failure rate 0–100 whole percent on one fixed test proxy |
| `POST /scenario?name=baseline\|latency_ladder\|reliability_gradient\|combined` | Apply a named bounded fault preset across the five test proxies |
| `POST /kill-proxy3`, `POST /delay-proxy2`, `POST /restore` | Stop Proxy 3, set 100 ms delay on Proxy 2, or resume Proxy 3 and restore baseline faults |
| `POST /start-load`, `POST /stop-load`, `GET /load-status` | Start/stop the bundled load generator and read its state |
| `POST /run-benchmark`, `GET /benchmark-results` | Start the Phase 6 benchmark and poll for status/results |

The dashboard reads each gateway proxy's configured host and port from gateway state. It does not permit arbitrary endpoint edits or configure third-party proxy providers.
| `success_rate` | number 0..1 | successful / (successful + failed); 1.0 when none yet | Phase 1 |
| `active_connections` | int | Connections currently in use | Phase 5 |
| `upstream_connections_opened`, `upstream_connections_reused` | int | Upstream sockets opened and reused by the Phase 5 forwarder | Phase 5 |
| `upstream_connect_timeouts`, `upstream_response_timeouts` | int | Phase 5 connect and response deadline expirations | Phase 5 |
| `upstream_bytes_in`, `upstream_bytes_out` | int | Bytes received from and sent to upstream proxies | Phase 5 |
| `upstream_active_connections`, `upstream_idle_connections` | int | Live upstream sockets and idle pooled sockets | Phase 5 |
| `worker_count` | int | Worker threads | Phase 2 |
| `aimd_limit` | int | Current concurrency limit C (Phase 1 shows `workers.max`) | Phase 2 |
| `queue_length` | object | Domain name to queued jobs, for example `{"site-a.example": 12}` | Phase 2 |
| `latency_ms.p50/p95/p99` | number | Percentiles over the latest 4096 samples (D-016) | Phase 1 |
| `gateway_overhead_ms.p50/p95/p99` | number | Percentiles of GatewayCore time excluding summed forwarder attempt time | Phase 5 |

### `GET /metrics` (Prometheus text, `Content-Type: text/plain; version=0.0.4`)

Metric names include `health_probe_successes_total`, `health_probe_failures_total`, per proxy `proxy_probe_successes_total`, `proxy_probe_failures_total`, `proxy_consecutive_failures`, `proxy_circuit_open_count`, `proxy_last_probe_unix_ms`, `proxy_rolling_rtt`, Phase 5 upstream connection, byte and timeout counters, and the `gateway_overhead_ms` summary; existing request and routing metrics remain available.

### `GET /ui`

HTML. Phase 1 served a placeholder page; Phase 6 serves the dashboard from `dashboard/`.

Other paths return 404 `{"error":"not found"}`.

## Test-bed interfaces

| Item | Detail |
| --- | --- |
| Target | `GET /products/<id>` (deterministic page), `GET /api/offers/<sku>` (synthetic PriceLens offer JSON keyed by retailer Host), `GET /health`, `GET /stats`; `?delay_ms=N` on product paths. Hosts include legacy test names and `northstar.pricelens.test`, `harbor.pricelens.test`, `copperlane.pricelens.test` |
| Proxy nodes | Five forward proxies on 3128–3132 (absolute-URL requests and CONNECT). Response header `X-Proxy-Id`. Faults: env `DELAY_MS`, `FAIL_PCT` (injected failures are HTTP 502 `injected failure`), optional `FAIL_SEED` |
| Proxy control port | Ports 3228–3232; `GET /fault?delay_ms=N&fail_pct=P` sets faults at run time; `GET /fault` shows them |
| Phase 6 control service | `127.0.0.1:9100`; routes and bounds are listed above; controls the bundled test bed only; `GET /pricelens-summary` reads the latest synthetic offer scan |
| Host ports (127.0.0.1) | Docker: target 18080 (env `TARGET_HOST_PORT`), proxies 3128–3132, control 3228–3232. Local mode: target 8081, others the same |

## Change history

- 2026-11-01 D-022: Latency policy exploration of eligible proxies without an RTT sample documented.
- 2026-11-01 D-023: Phase 2 verifier isolates rate/AIMD checks from newly active Phase 3 routing.
- 2026-11-02 D-024 to D-026: Phase 4 rolling health, probe/circuit behavior, separate probe counters and bounded per-proxy probe threads documented.
- 2026-11-02 D-027: Routing health uses a four-outcome tail so recovered proxies can re-enter promptly while the longer rolling metric remains stable.
- 2026-11-02 D-028: Phase 2 verifier waits for health recovery and raises its test-only circuit threshold to keep the AIMD check independent.
- 2026-11-02 D-029: Phase 5 keeps the existing HTTP-only ingress contract and defers CONNECT tunneling.
- 2026-11-02 D-030: Phase 5 uses one level-triggered reactor and a bounded per-proxy keep-alive pool.
- 2026-11-02 D-031: Phase 5 adds upstream pool gauges and connection/byte/timeout metrics through `MetricsSink`.
- 2026-11-02 D-032: Phase 5 makes pool bounds configurable with a global socket cap.
- 2026-11-02 D-033: Phase 5 exposes bounded gateway-core overhead percentiles.
- 2026-11-02 D-034: Test proxy enables TCP_NODELAY for persistent Phase 5 measurements.
- 2026-11-02 D-035: The global pool cap evicts the oldest idle connection when necessary.
- 2026-11-02 D-036: Phase 6 bounded background result output and URL redaction are documented.
- 2026-11-02 D-037: Phase 6 runtime policy and default rate controls are documented.
- 2026-11-02 D-038: Phase 6 local-only test-bed controls and benchmark routes are documented.
- 2026-11-02 D-040: Phase 6 health-aware selection experiment toggle retains circuit exclusions.
- 2026-11-03 D-041: Dashboard can change bounded test-proxy delay and failure injection and apply named presets.
- 2026-11-03 D-042: PriceLens demo monitors deterministic synthetic offers from three fictional retailer hosts.
- 2026-11-03 D-043: Demo test bed expands to five independently configurable local proxies with latency and failure presets.
- 2026-11-02 D-032: Phase 5 adds configurable per-proxy and global pool limits plus idle eviction.
- 2026-11-02 D-033: Phase 5 reports bounded gateway-core overhead percentiles.
- 2026-11-02 D-034: Test proxy sets TCP_NODELAY to avoid a persistent-response timing artifact.
- 2026-11-01 D-021: Retry exhaustion preserves the last transport error mapping.
- 2026-11-01 D-020: Phase 3 routing policies, eligibility, weighted normalization, RTT smoothing and retry semantics documented.
- 2026-11-01 D-019: Phase 2 scheduler, async listener ownership handoff, bounded worker pool, AIMD policy and `queue_rejected` counter added.
- 2026-11-01 D-015: Admin API shapes (`/health`, `/proxies`, `/api/stats`, `/metrics`) defined with the Phase 1 stub.
- 2026-11-01 D-009: Config keys and CLI defined.
- 2026-11-01 D-010: Success/failure counter definitions.
- 2026-11-01 D-011, D-012: Test-bed interfaces defined.
