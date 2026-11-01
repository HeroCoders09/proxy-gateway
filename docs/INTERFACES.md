# Interfaces

Source of truth for shared structs, module interfaces, config keys and the admin API. Changing anything here needs a decision entry in `docs/DECISIONS.md` and an entry in the change history at the bottom, in the same commit. Fields may be added; they may not be renamed or removed without a decision.

Code locations: structs in `src/common/types.hpp`, interfaces in `src/common/interfaces.hpp`, metrics in `src/common/metrics.hpp`, config in `src/common/config.hpp`, clock in `src/common/clock.hpp`, descriptor wrapper in `src/common/fd.hpp`. All in namespace `pg`.

## Structs

| Struct | Fields (type) | Notes |
| --- | --- | --- |
| `Job` | `id` (u64), `url` (string, as received), `domain` (string, lower-case host without port: the queue and rate-limit key), `method`, `headers` (vector of `{name,value}`), `body`, `enqueue_time` (steady `TimePoint`), `retry_count` (int), `tried_proxies` (vector of `ProxyId`), `client_fd` (int, NOT owned), plus parsed `target_host`, `target_port` (u16, default 80), `target_path` (path and query, starts with `/`) | `client_fd` stays valid only while the ingress owns the connection; Phase 2 must keep the connection alive until the reply is written |
| `ProxyState` | `id` (`ProxyId` = int, index in config `proxies`), `host`, `port`, `healthy` (bool, default true), `rtt_ms` (double), `success_count`, `failure_count` (u64), `consecutive_failures` (u32), `active_connections` (int), `last_probe_time`, `circuit_state` (`CircuitState`: `Closed`, `Open`, `HalfOpen`; JSON strings `CLOSED`, `OPEN`, `HALF_OPEN`) | Phase 3 builds the thread-safe table; Phase 4 drives `healthy` and `circuit_state` |
| `DomainQueue` | `jobs` (deque of `Job`), `queue_limit` (default 10000), `tokens`, `refill_rate` (tokens per second), `capacity` (burst B), `last_refill` | Phase 2 |
| `Connection` | `fd` (`Fd`), `proxy_id`, `state` (`ConnState`: `Idle`, `Connecting`, `Active`, `Closed`), `last_used`, `requests_served`, `keep_alive`, `in_buf`, `out_buf` | Phase 5 |
| `Metrics` (implements `MetricsSink`) | counters `RequestsTotal`, `RequestsSuccessful`, `RequestsFailed`, `Timeouts`, `BadRequests`; bounded latency ring (4096 samples) giving p50, p95, p99 | Each phase adds its own counters and gauges here (handbook rule 7) |
| `GatewayConfig` | see "Config file keys" | Loaded and validated by `load_config_file` |
| `HttpResponse` (`src/ingress/http_response.hpp`) | `status` (int), `headers`, `body` | Used for every client reply; `write_response(fd, resp, head_only, timeout_ms)` sends it |
| `ForwardResult` | `error` (`ErrorKind`), `error_detail` (short string, never a body or credential), `status`, `headers`, `body`, `latency_ms` (connect + request + response), `proxy_id`; `ok()` is true when `error == None` | |

`ErrorKind`: `None`, `ConnectTimeout`, `ConnectFailed`, `ResponseTimeout`, `IoError`, `BadResponse`. Strings (`to_string`): `none`, `connect_timeout`, `connect_failed`, `response_timeout`, `io_error`, `bad_response`. `is_timeout(kind)` is true for the two timeout kinds. Phase 3 retry logic keys off this.

## Module interfaces

| Interface | Signature and contract |
| --- | --- |
| `Forwarder` | `ForwardResult forward(const Job& job, const ProxyState& proxy)`. Thread-safe: may be called from many threads. Never throws for network problems; they come back as `ForwardResult.error`. Implemented by `SimpleForwarder` (Phase 1, `src/net/simple_forwarder.*`); Phase 5's epoll forwarder implements the same interface and is chosen by config `forwarder` |
| `Router` | `std::optional<ProxyId> select(const Job& job, const std::vector<ProxyState>& table)`. Returns `nullopt` when no route is available. Phase 1 has no router (the gateway uses proxy 0); Phase 2 adds a stub, Phase 3 the real policies |
| `RateLimiter` | `bool try_acquire(const std::string& domain)`. Must use a `Clock` that tests can fake. Phase 2 |
| `HealthReporter` | `void report(ProxyId proxy_id, bool success, double rtt_ms)`. Called after every attempt and every probe. Phase 3 |
| `MetricsSink` | `void inc(Counter c, uint64_t n = 1)`, `void observe_latency_ms(double ms)`. Thread-safe; the only way modules write measurements. Phase 2 to 5 add counters to the `Counter` enum (append before `kCount`; never reorder) |
| `Clock` | `TimePoint now() const` on `std::chrono::steady_clock`. `SteadyClock` for production, `FakeClock` (`advance(d)`) for tests |
| `Fd` | RAII file-descriptor owner: `get()`, `valid()`, `release()`, `reset(fd)`, move-only. No raw `close()` anywhere else |
| `Listener` | `Listener(cfg, MetricsSink&, Handler)` where `Handler = std::function<HttpResponse(Job&)>`; `start()`, `stop()`, `port()`. Phase 1 handler runs the whole forward synchronously (see D-005) |

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
| `policy` | `random`, `roundrobin`, `leastconn`, `latency`, `weighted` | `roundrobin` | Routing policy (later: Phase 3). Overridden by `--policy` |
| `forwarder` | `simple`, `epoll` | `simple` | Forwarder implementation. `epoll` is rejected at start-up until Phase 5 |
| `rate_limits.default.rate` | number > 0 | 1000 | Token refill per second for domains with no entry (later: Phase 2) |
| `rate_limits.default.burst` | number > 0 | 2000 | Bucket capacity B |
| `rate_limits.<domain>.rate` / `.burst` | number > 0 | inherit default | Per-domain override; domain names are lower-cased |
| `queue_limit` | int 1-10,000,000 | 10000 | Per-domain queue limit; overflow gives 503 (later: Phase 2) |
| `workers.min` / `workers.max` | int 1-100000 | 4 / 64 | AIMD bounds; `max` must be at least `min` (later: Phase 2). `--workers N` overrides `max` |
| `workers.alpha` | number > 0 | 1 | Additive increase |
| `workers.beta` | number, 0 < beta < 1 | 0.5 | Multiplicative decrease |
| `timeouts.connect_ms` | int 1-3,600,000 | 2000 | Connect timeout per attempt |
| `timeouts.response_ms` | int 1-3,600,000 | 10000 | Response timeout per attempt, measured from connect completion |
| `circuit.threshold` | int >= 1 | 5 | Consecutive failures that open a circuit (later: Phase 4) |
| `circuit.cooldown_ms` | int 1-3,600,000 | 10000 | OPEN to HALF_OPEN cooldown |
| `weights.latency` / `.load` / `.failure` | number >= 0 | 0.5 / 0.3 / 0.2 | Weighted-score policy weights (later: Phase 3) |
| `ingress.threads` | int 1-4096 | 16 | Fixed handler-thread pool |
| `ingress.max_header_bytes` | int 256 to 1 MiB | 16384 | Larger request heads get 400 |
| `ingress.max_body_bytes` | int >= 1 | 1048576 | Larger bodies get 400 |
| `ingress.read_timeout_ms` | int > 0 | 5000 | Time a client has to deliver a full request |
| `ingress.pending_connections` | int >= 1 | 1024 | Bound on accepted-but-unserved connections; overflow gets 503 |
| `output.format` | `none`, `csv`, `json` | `none` | Result writer (Phase 6) |
| `output.path` | string | empty | Required when `format` is not `none` |

CLI: `gateway --config FILE [--policy NAME] [--workers N] [--help]`. `--config` is required. Exit code 0 on `--help`, 2 for any configuration or CLI error (message `gateway: error: ...` on stderr), 1 if a port cannot be bound.

Ready-made configs: `config/gateway.json` (documented example, hosts `proxy1` to `proxy3`), `config/gateway.local.json` (gateway on the host, proxies on `127.0.0.1:3128-3130`), `config/gateway.docker.json` (gateway container, admin listens on `0.0.0.0` inside the container only).

## Gateway-generated responses

| Status | When |
| --- | --- |
| 400 | Malformed, oversized or incomplete request (see D-007) |
| 502 | Connect failure, I/O error or unparseable proxy response, handler exception (later also: no route) |
| 503 | Ingress pending queue full (later: domain queue full) |
| 504 | Connect or response timeout |
| 429 | Reserved |

All are `text/plain` with a short fixed message. Proxied responses keep the target's status and gain `X-Gateway-Proxy: <proxy id>`.

## Admin API

Served on `admin.host:admin.port` (default `127.0.0.1:9000`), GET only (other methods get 405 JSON). JSON responses use `Content-Type: application/json`. Phase 1 serves dummy data plus the real Phase 1 counters (`"stub": true`); Phase 6 serves live data under the same field names and sets `"stub": false`. Dashboard code must ignore unknown fields.

Units: times in milliseconds unless the name says otherwise; `_unix_ms` is milliseconds since the Unix epoch; rates are per second; `success_rate` is a fraction from 0 to 1.

### `GET /health`

```json
{"status": "ok", "uptime_seconds": 12.5, "version": "0.1.0", "phase": 1, "stub": true}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `status` | string | `"ok"` while the process is serving |
| `uptime_seconds` | number | Seconds since start |
| `version` | string | Gateway version |
| `phase` | int | Latest completed phase (informational) |
| `stub` | bool | true until Phase 6 |

### `GET /proxies`

```json
{"stub": true, "proxies": [
  {"id": 0, "host": "127.0.0.1", "port": 3128, "healthy": true, "rtt_ms": 0.0,
   "success_count": 0, "failure_count": 0, "consecutive_failures": 0, "success_rate": 1.0,
   "active_connections": 0, "circuit_state": "CLOSED", "circuit_open_count": 0, "last_probe_unix_ms": 0}
]}
```

| Field | Type | Meaning | Filled by |
| --- | --- | --- | --- |
| `id` | int | Index in config `proxies` | Phase 1 |
| `host`, `port` | string, int | Proxy address | Phase 1 |
| `healthy` | bool | Derived health flag | Phase 4 |
| `rtt_ms` | number | Recent smoothed RTT | Phase 3 |
| `success_count`, `failure_count` | int | Attempts through this proxy | Phase 3 |
| `consecutive_failures` | int | Current failure streak | Phase 3 |
| `success_rate` | number 0..1 | Rolling success rate | Phase 4 |
| `active_connections` | int | In-flight requests through this proxy | Phase 3 |
| `circuit_state` | string | `CLOSED`, `OPEN` or `HALF_OPEN` | Phase 4 |
| `circuit_open_count` | int | Times the circuit has opened | Phase 4 |
| `last_probe_unix_ms` | int | Time of the last health probe, 0 if none | Phase 4 |

### `GET /api/stats` (dashboard stats endpoint)

```json
{"stub": true, "timestamp_unix_ms": 1790000000000, "policy": "roundrobin",
 "requests_total": 0, "requests_successful": 0, "requests_failed": 0, "timeouts": 0,
 "requests_per_second": 0.0, "success_rate": 1.0, "active_connections": 0, "worker_count": 0,
 "aimd_limit": 64, "queue_length": {}, "latency_ms": {"p50": 0.0, "p95": 0.0, "p99": 0.0}}
```

| Field | Type | Meaning | Real from |
| --- | --- | --- | --- |
| `timestamp_unix_ms` | int | Server time of the snapshot | Phase 1 |
| `policy` | string | Active routing policy | Phase 1 (value from config) |
| `requests_total`, `requests_successful`, `requests_failed`, `timeouts` | int | Counters (definitions in D-010) | Phase 1 |
| `requests_per_second` | number | Rate over the last few seconds | Phase 6 |
| `success_rate` | number 0..1 | successful / (successful + failed); 1.0 when none yet | Phase 1 |
| `active_connections` | int | Connections currently in use | Phase 5 |
| `worker_count` | int | Worker threads | Phase 2 |
| `aimd_limit` | int | Current concurrency limit C (Phase 1 shows `workers.max`) | Phase 2 |
| `queue_length` | object | Domain name to queued jobs, for example `{"site-a.example": 12}` | Phase 2 |
| `latency_ms.p50/p95/p99` | number | Percentiles over the latest 4096 samples (D-016) | Phase 1 |

### `GET /metrics` (Prometheus text, `Content-Type: text/plain; version=0.0.4`)

Metric names (Phase 1 serves real values for the first five and the latency summary): `requests_total`, `requests_successful`, `requests_failed`, `timeouts`, `bad_requests` (counters); `active_connections`, `requests_per_second`, `worker_count`, `queue_length` (gauges, 0 until later phases); `latency_ms{quantile="0.5"|"0.95"|"0.99"}`; per proxy with label `proxy="<id>"`: `proxy_rtt`, `proxy_success_rate`, `proxy_failure_count`. Later phases add metrics by appending; names above stay.

### `GET /ui`

HTML. Phase 1 serves a placeholder page; Phase 6 serves the dashboard from `dashboard/`.

Other paths return 404 `{"error":"not found"}`.

## Test-bed interfaces

| Item | Detail |
| --- | --- |
| Target | `GET /products/<id>` (deterministic page containing the id), `GET /health` (`{"status":"ok"}`), `GET /stats` (request counts per Host header, with per-second counts), `?delay_ms=N` on any path. Host names `test.example`, `site-a.example`, `site-b.example` |
| Proxy nodes | Forward proxy on 3128 (absolute-URL requests and CONNECT). Response header `X-Proxy-Id`. Faults: env `DELAY_MS`, `FAIL_PCT` (injected failures are HTTP 502 `injected failure`), optional `FAIL_SEED` |
| Proxy control port | `GET /fault?delay_ms=N&fail_pct=P` sets faults at run time; `GET /fault` shows them |
| Host ports (127.0.0.1) | Docker: target 8080, proxies 3128/3129/3130, control 3228/3229/3230. Local mode: target 8081, others the same |

## Change history

- 2026-11-01 D-015: Admin API shapes (`/health`, `/proxies`, `/api/stats`, `/metrics`) defined with the Phase 1 stub.
- 2026-11-01 D-009: Config keys and CLI defined.
- 2026-11-01 D-010: Success/failure counter definitions.
- 2026-11-01 D-011, D-012: Test-bed interfaces defined.
