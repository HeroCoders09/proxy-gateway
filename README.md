# Intelligent Concurrent Proxy Gateway

C++20 / POSIX / epoll forward-proxy gateway that load-balances HTTP, HTTPS (CONNECT) and request bodies over a pool
of upstream proxies, with per-domain token-bucket scheduling, AIMD concurrency control, five routing policies,
circuit breakers, health probes, retries, queue timeouts and Prometheus-style metrics.

## Project in brief

This is a self-hosted forward proxy. A client sends a request to the gateway; the gateway chooses one of its configured upstream proxies, forwards the request, and returns the response. It can monitor proxy health, route around failures, limit traffic per destination, and show request and latency metrics. HTTPS uses a CONNECT tunnel, so the gateway relays encrypted traffic without inspecting its contents.

The repository includes a local target server and simulated proxy nodes for safe, repeatable demos. You can also configure reachable external HTTP or SOCKS5 proxies in the dashboard.

## Requirements

- Linux (the gateway uses `epoll`) with a C++20 compiler and CMake 3.16 or newer.
- Python 3 and Bash for the dashboard and launcher scripts; `nproc` is used to choose the build parallelism.
- A browser for the dashboard. Chromium/Chrome can open it as an app window; otherwise the UI opens in the default browser.
- For the local demo, ports `8000`, `8101`, `8102`, `8103`, and `9000` must be available.
- To run the full CTest end-to-end suite, also install `curl` and the OpenSSL command-line tools.

The launcher scripts build the required binaries automatically.

## Quick start: local demo

From the repository root, run:

```bash
./scripts/run_local_demo.sh
```

This builds the project, starts the local target on port `9000` and three simulated proxies on ports `8101`–`8103`, then opens the dashboard with a temporary local-demo configuration. In the dashboard, click **Start**, open **Test request**, and use the default target `http://127.0.0.1:9000/health`. Click **Send** for one request or use **Load test** to send a paced series. The local test proxies have different delays and injected failure rates, so their health and traffic results can be compared.

Press **Ctrl+C** in the terminal running the script to stop the dashboard and the local demo services. The script reports a port conflict if one of its required ports is already listening.

## Quick start: saved real upstream proxies

First, run `python3 app/gateway_ui.py`, then add your proxy endpoints in **Proxies**, including the protocol and any required credentials. Save the configuration and close that dashboard. By default, the dashboard stores it at `~/.config/proxy-gateway/gateway.json`.

Then run:

```bash
./scripts/run_real_proxy_demo.sh
```

This builds and starts the gateway with that saved configuration, then opens the dashboard attached to it. The external proxy services must already be running and reachable; this script starts the gateway but cannot start proxy servers hosted by a provider. Press **Ctrl+C** in the script's terminal to close the dashboard and stop the gateway.

To use another saved config file:

```bash
GATEWAY_CONFIG=/path/to/gateway.json ./scripts/run_real_proxy_demo.sh
```

For a request through those proxies, open **Test request** and enter a public HTTP URL such as `http://httpbin.org/get`. The dashboard's **Load test** can also use public HTTP hosts on port 80; public runs are capped at 10 requests/second, 60 seconds, and 600 requests. Loopback targets such as `127.0.0.1:9000` require local proxy nodes, because a remote upstream proxy cannot reach your computer's loopback interface.

To stop a gateway left running by another command, find its listener and stop the PID:

```bash
ss -ltnp 'sport = :8000'
kill <PID>
```

## Features
| Area | Behaviour |
|---|---|
| Methods | GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS with request bodies (Content-Length **and** chunked), `Expect: 100-continue` |
| HTTPS | `CONNECT host:port` tunnels (retried across proxies, relayed by the epoll I/O loops with back-pressure, half-close aware, idle timeout) |
| Client connections | HTTP/1.1 keep-alive, pipelining, partial/dribbled requests, idle + slow-loris timeouts, header/body size limits |
| Scheduling | per-domain FIFO + token bucket; a dispatcher sleeps on a condition variable (**0 % idle CPU**); `timeouts.queue_ms` expires queued requests with 504 |
| Concurrency | AIMD limit is a hard bound on simultaneously executing requests |
| Upstream proxies | per-proxy `"protocol": "http"` (default; absolute-URL forwarding + `CONNECT`) or `"socks5"` (RFC 1928; no-auth or RFC 1929 username/password; domain names resolved by the proxy); both can be mixed and fail over to each other. SOCKS5 tunnels are pooled per destination; health probes use the proxy's own protocol |
| Resilience | health probes, circuit breaker (CLOSED→OPEN→HALF-OPEN→CLOSED on live traffic), retries (non-idempotent requests are only retried if nothing was sent) |
| Config | strict JSON parser, any whitespace/key order, typed + range validation, errors with line:column, `--check-config` |
| Logs | CSV and JSONL are escaped correctly (quotes, commas, control chars, invalid UTF-8) |

## Build & test
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$(nproc)"
    ctest --test-dir build --output-on-failure  # unit tests, parser fuzz, and end-to-end suite

Individual suites:

    ./build/unit_tests
    python3 scripts/e2e_test.py --build build [--only tls] [--docker]

## Manual local test bed (optional)

Use this when you want to start the individual demo components yourself instead of using `run_local_demo.sh`. Start each server in a separate terminal, then use the curl commands below.

    ./build/target_server --port 9000 &
    ./build/proxy_node --port 8101 --delay-ms 0   --fail-rate 0.0 &
    ./build/proxy_node --port 8102 --delay-ms 100 --fail-rate 0.0 &
    ./build/proxy_node --port 8103 --delay-ms 10  --fail-rate 0.20 &
    ./build/gateway --config config/gateway.json --policy weighted --workers 16

    curl --noproxy '' -x http://127.0.0.1:8000 http://127.0.0.1:9000/products/123
    curl --noproxy '' -x http://127.0.0.1:8000 -d 'hello' http://127.0.0.1:9000/upload
    curl --noproxy '' -x http://127.0.0.1:8000 -k https://example.org/        # CONNECT tunnel
    curl --socks5-hostname 127.0.0.1:8101 http://127.0.0.1:9000/x            # proxy_node speaks SOCKS5 too
    curl http://127.0.0.1:8000/proxies ; curl http://127.0.0.1:8000/metrics ; curl http://127.0.0.1:8000/health   # admin: localhost only

> **NO_PROXY gotcha:** if your shell has `NO_PROXY`/`no_proxy` containing `127.0.0.1`, curl silently ignores `-x`
> and talks to the target directly. Pass `--noproxy ''` (as above) so the request really goes through the gateway.

## Configuration
See `config/gateway.json` (documented keys). New optional keys:
`listen_host`, `timeouts.queue_ms | client_idle_ms | client_header_ms | tunnel_idle_ms`,
`limits.max_header_bytes | max_body_bytes | max_response_bytes | max_client_connections | pool_max_idle_per_proxy | pool_max_idle_total`.
Each proxy entry takes an optional protocol, e.g. `{"host": "10.0.0.5", "port": 1080, "protocol": "socks5"}`
(`"http"` is the default; anything else is rejected at startup). SOCKS5 proxies that need a login take
`"username"` and `"password"` (both required, 1..255 bytes, only valid with `"protocol": "socks5"`); the gateway offers both
"no authentication" and username/password and uses whichever the proxy picks. Credentials are never logged or shown by `/proxies`, and
are also used by the health probes. `proxy_node --only socks5 --auth user:pass` is a matching test server. `/proxies` reports the protocol of every proxy.
`admin` (see *Security & addressing*), `proxies[].protocol | username | password | password_env | password_file`.
Validate any file with `./build/gateway --config my.json --check-config`.
`config/gateway.docker.json` uses the compose service names (`proxy1..3`, `target`) as hostnames.

## Docker
    export GATEWAY_ADMIN_TOKEN=$(./build/gateway --gen-admin-token)
    docker compose up --build        # target + 3 proxy nodes + gateway: proxy on :8000, admin (token) on 127.0.0.1:8001

## Benchmarks & load tests
    ./scripts/run_benchmarks.sh                    # compares the five routing policies -> benchmark_summary.csv
    ./scripts/run_loadtest.sh 256 15               # 256 keep-alive connections for 15 s (loadgen)
    ./scripts/run_loadtest.sh 128 10 POST 4096     # POST with 4 KB bodies
    ./scripts/kernel_tune.sh apply | revert        # sysctl tuning for very high concurrency (needs root)

## Cleanup / reset
    pkill -f build/gateway; pkill -f build/proxy_node; pkill -f build/target_server
    rm -rf build results.csv results.jsonl benchmark_summary.csv
    ./scripts/kernel_tune.sh revert                # only if you ran "apply"
    docker compose down --rmi local --volumes      # only if you used Docker

## Security & addressing
* **IPv4 and IPv6** everywhere: `listen_host` (**`"127.0.0.1"` default**: the forward proxy has no client authentication, so exposing it makes an open proxy; `"0.0.0.0"` / `"::"` (IPv6 + IPv4 dual stack) are explicit opt-ins that print a `[Security] WARNING`), proxy hosts
  (names or literals, `"::1"` / `"[::1]"`), origins (`http://[2001:db8::1]:8080/`, `CONNECT [::1]:443`) and SOCKS5 destinations (ATYP 4).
  Proxy names are resolved at startup and refreshed by the prober, never on the request path. A name that resolves to several addresses (e.g. `localhost` = `::1` and `127.0.0.1`) is tried address by address: connects, health probes, and the `connect_with_timeout` helper used by `proxy_node`/`loadgen` fall over to the next address, and the one that worked is tried first next time. `proxy_node` and `target_server` take `--listen ADDR`.
* **Secrets out of the config file:** every secret can be written inline (`"password"`, `"token"`), read from an environment variable
  (`"password_env": "NAME"`, `"token_env"`) or from a file (`"password_file": "/run/secrets/x"`, `"token_file"`); give exactly one.
  A missing variable / unreadable or empty file is a startup error. If a secret is written inline and the config file is readable by
  group/others, the gateway prints a warning (also from `--check-config`) telling you to `chmod 600` it or use `_env` / `_file`.
* **Admin endpoints (`/metrics`, `/proxies`, `/health`) are local-only by default.** On the proxy port they answer loopback clients only
  (others get `403`); the proxy service itself is unaffected. Options (`"admin": {...}`):
  * `"port": 8001` – serve the admin endpoints ONLY on a dedicated listener bound to `listen_host` (default `127.0.0.1`).
    That listener never proxies anything, and the proxy port then answers `404` for admin paths.
  * `"token"` / `"token_env"` / `"token_file"` – require `Authorization: Bearer <token>` for `/metrics` and `/proxies` (constant-time
    compare, `401` + `WWW-Authenticate` otherwise). `/health` stays open for probes but then reports only `{"status":"healthy"}`.
  * Exposing the admin interface to other machines (`"allow_remote": true`, or a non-loopback `listen_host` such as `"0.0.0.0"`)
    **is refused at startup unless a token is configured**, and prints a warning because the token then travels over plain HTTP.
  * Tokens are never hard-coded: `gateway --gen-admin-token` prints 256 random bits (hex); `gateway --gen-admin-token FILE` writes them to
    a new file created with mode `600` (never overwrites). Use it with `"token_file"`. Secret files readable by group/others trigger a warning.
  * There is no TLS listener. If you must administer remotely, keep the port on a private interface (VPN / WireGuard / SSH tunnel) or put a
    TLS reverse proxy in front. The Docker setup publishes the admin port on the host's loopback only (`127.0.0.1:8001`) and requires
    `GATEWAY_ADMIN_TOKEN` (compose refuses to start without it).
* **SOCKS5 upstreams:** credentials are never logged or printed (tests scan every process log). At start-up and in `--check-config`, a
  SOCKS5 proxy that resolves outside loopback/private networks (RFC 1918, link-local, 100.64/10, IPv6 ULA) gets a `[Security] WARNING`: its
  traffic, and the RFC 1929 password, would cross the network unencrypted. Reach such proxies through WireGuard or an SSH tunnel
  (e.g. `ssh -N -L 127.0.0.1:1080:proxy-host:1080 user@host`, then point the gateway at `127.0.0.1:1080`).

## Desktop app (Linux)

`bash scripts/install_desktop.sh install` builds the gateway if needed and adds a **Proxy Gateway** launcher to the app menu
(`uninstall` removes it). Or run `python3 app/gateway_ui.py` directly (stdlib only; `--bin PATH`, `--config PATH`, `--no-open`, `--keep-running`).
It opens an app window (Chromium/Chrome in `--app` mode, else the default browser) with: start / stop / restart, a live dashboard
(throughput, latency percentiles, per-proxy health and circuit state, idle pool, open files), forms for settings, proxies, rate limits and
the admin interface, config validation with the gateway itself before saving, admin-token generation (0600 file), a test-request tool and
the log. Config lives in `~/.config/proxy-gateway/gateway.json` (mode 600; stored passwords are never shown again). The UI server binds to
127.0.0.1 only, checks the Host header and requires a per-launch session cookie.

The **Test request** tab sends one request through the gateway. Its **Load test** panel sends a paced stream and shows throughput, outcomes and latency. See **Quick start: local demo** and **Quick start: saved real upstream proxies** above for run instructions and target limits.

## File descriptors

Every client connection, in-flight upstream request, CONNECT tunnel (2) and **idle pooled upstream connection** holds a descriptor.
`limits.pool_max_idle_per_proxy` (default 64) and `limits.pool_max_idle_total` (default 1024; `0` per proxy disables pooling) bound the pool.
At start-up the gateway raises its soft `RLIMIT_NOFILE` to the hard limit and then clamps `pool_max_idle_total` (to a quarter of the usable
descriptors) and `max_client_connections` to what fits, printing a `[Limits]` note for each change; raise `ulimit -n` / `LimitNOFILE` for
large deployments. `/metrics` exports `upstream_idle_connections`, `process_open_fds` and `process_fd_limit`.

## Testing & hardening

- `ctest` runs unit tests, the quick parser fuzz and the e2e suite (37 tests).
- `build/fuzz_parsers [iters] [seed]` - mutation fuzzing of the HTTP/chunked/SOCKS5/JSON/config parsers with invariants.
- `python3 scripts/fuzz_network.py --build build [--rounds N] [--seed S]` - hostile-peer network fuzzing.
- Sanitizers: configure with `-DCMAKE_CXX_FLAGS="-O1 -g -fsanitize=address,undefined"` or `-fsanitize=thread`, then run `unit_tests` and `scripts/e2e_test.py --build <dir>`.
- Config: `timeouts.upstream_idle_ms` (100..3600000, default 30000) closes idle pooled upstream connections.
- Bugs found by this campaign and fixed: a cross-thread data race (TSan), chunked responses with trailers never completing, lenient status-line parsing, idle pooled connections never reaped.
- Not verifiable in the dev sandbox: IPv6 sockets and Docker (those tests skip).

## Remaining limitations
* SOCKS5 upstreams: no-auth and username/password (RFC 1929) with the CONNECT command (no GSSAPI, no UDP ASSOCIATE). RFC 1929 itself is
  unencrypted: the gateway warns, but only a tunnel (WireGuard / SSH) can protect it.
* No TLS on the gateway's own listeners (proxy and admin). The admin token is therefore only safe on loopback / a private network / a VPN.
* Secrets are read from env vars or files; there is no integration with the Linux keyring or a secret manager.
* IPv6 was implemented against the same address abstraction as IPv4 (so the IPv4 test suite covers the shared code), but the sandbox used
  for development has no IPv6 stack: `test_ipv6_end_to_end` is skipped there and runs on IPv6-capable hosts. Run it once in your environment.

## Thread model

Non-tunnel requests are handled by an event-driven engine (epoll): connect, send, read, retry and the reply to the client are
state machines driven by a few I/O threads, with timers for connect/response/write timeouts. No thread waits on the network for a
request, so thousands of requests can be in flight with a handful of threads.

* `workers.min/initial/max` bound the number of requests **in flight** (AIMD adjusts it); they are no longer thread counts.
* `workers.io_threads` sets the I/O threads (0 = auto, min(4, cores)).
* One dispatcher thread feeds the engine from the rate-limited queue; one prober thread checks proxy health.
* Live CONNECT tunnels are relayed by the same I/O threads (non-blocking, one buffer per direction for back-pressure, `tunnel_idle_ms` timer), so they use no threads either.
