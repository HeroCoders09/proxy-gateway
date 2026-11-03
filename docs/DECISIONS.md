# Decisions log

Append-only. Number decisions D-001, D-002 and so on. Never edit an old entry; supersede it with a new one.

> Provenance note (Phase 1): D-001 to D-010 describe choices already present in the code when this log was created on 2026-11-01. They were reconstructed by reading the code and tests, and D-010 is also cited in source comments. If any entry misstates intent, supersede it with a new entry rather than editing it.

## D-001: Unit-test framework is Catch2 v3 (amalgamated, vendored)
- Date: 2026-11-01
- Phase: 1 (all later phases write tests with it)
- Made by: Phase 1 owner
- Decision: Use Catch2 v3 as a single amalgamated header/source pair vendored in `third_party/catch2/`. Tests are `tests/*.cpp` (picked up by a CMake glob) linked into one `pg_tests` binary, run by `ctest` or `./build/pg_tests`.
- Why: No system package or network access is needed to build; one extra translation unit; tags and filters make it easy to run one module's tests.
- Alternatives considered: GoogleTest (needs a system install or FetchContent, which fails offline); hand-rolled asserts (no filtering or reporting).
- Consequences: New test files only need to be dropped into `tests/`; re-run CMake because the glob uses `CONFIGURE_DEPENDS`. The vendored file is compiled with `-w` so its internals never fail our `-Wall -Wextra` build.
- Status: Active

## D-002: JSON library is nlohmann/json 3.11.3 (single header, vendored)
- Date: 2026-11-01
- Phase: 1 (config loader, admin API)
- Made by: Phase 1 owner
- Decision: Vendor `third_party/nlohmann/json.hpp` (version 3.11.3). Include as `<nlohmann/json.hpp>`.
- Why: Named in the handbook stack; header only; no build-time download.
- Alternatives considered: RapidJSON, Boost.JSON (extra dependency, no benefit at this scale).
- Consequences: Do not add a second JSON library.
- Status: Active

## D-003: Branch, commit and tag rules
- Date: 2026-11-01
- Phase: all
- Made by: Phase 1 owner
- Decision: Follow handbook rule 10. `main` holds only finished phases. Each phase works on `phase-N-short-name` (Phase 1: `phase-1-foundation`) and is merged to `main` when its done checklist is complete, then tagged `phase-N-done`. Commit messages say what changed and cite a decision ID when one applies, for example `Add token bucket (D-007)`.
- Why: Gives every later owner a known-good starting point.
- Alternatives considered: Trunk-based work on `main` (breaks the "start from a green system" rule).
- Consequences: Phase 1 has NOT been merged or tagged yet, because one checklist item (`docker compose up`) is still UNVERIFIED (see `docs/ISSUES.md` I-002). Merge and tag after that item is confirmed.
- Status: Active

## D-004: Fixed ports; admin binds to localhost; IPv4 only
- Date: 2026-11-01
- Phase: 1 (affects 6)
- Made by: Phase 1 owner
- Decision: Gateway client port 8000, admin and dashboard port 9000, control service port 9100 (Phase 6, test bed only). The admin server binds to `127.0.0.1` by default. Listen and admin hosts must be IPv4 literals.
- Why: Handbook section 3.5 and the SRS security requirement (admin and metrics on localhost).
- Alternatives considered: Binding admin to `0.0.0.0` (violates the security requirement); IPv6 support (not needed for the test bed).
- Consequences: Inside a container the admin server must listen on `0.0.0.0` to be reachable through a published port; `config/gateway.docker.json` does this and `docker-compose.yml` publishes it to `127.0.0.1` only (D-014).
- Status: Active

## D-005: Ingress threading model
- Date: 2026-11-01
- Phase: 1 (replaced in part by Phase 2)
- Made by: Phase 1 owner
- Decision: One accept thread feeds a bounded queue (`ingress.pending_connections`, default 1024) that a fixed pool of handler threads (`ingress.threads`, default 16) drains. When the queue is full the accept thread answers 503 immediately. In Phase 1 a handler thread parses the request, runs the whole forward synchronously, writes the reply and closes the connection.
- Why: Satisfies "no thread per request" and keeps Phase 1 simple.
- Alternatives considered: Thread per connection (violates the handbook); epoll ingress (more complex; not needed until performance work).
- Consequences: Phase 1 concurrency is capped at `ingress.threads` in-flight requests. Phase 2 must replace the synchronous `Handler` callback with "enqueue Job into its domain queue" and a worker pool, and write the reply from a worker through `write_response()` (see I-003).
- Status: Active

## D-006: Phase 1 forwarder design
- Date: 2026-11-01
- Phase: 1 (replaced by Phase 5; kept as reference)
- Made by: Phase 1 owner
- Decision: `SimpleForwarder` uses blocking-style sockets bounded by `poll()` deadlines. It resolves the proxy host, connects (connect timeout default 2000 ms), sends a forward-proxy request in absolute form with `Connection: close`, and reads the response (response timeout default 10000 ms, clock starting after connect). One connection per request. Responses over 16 MiB are rejected as `BadResponse`. It strips hop-by-hop headers in both directions. Chunked and read-until-close bodies are decoded.
- Why: Simplest correct implementation behind the fixed `Forwarder` interface; gives Phase 5 a reference to compare against.
- Alternatives considered: Using libcurl (hides the connection details the course is about); epoll now (Phase 5's job).
- Consequences: No connection reuse in Phase 1; a blocking `getaddrinfo` runs per request (I-008).
- Status: Active

## D-007: Request parsing rules at ingress
- Date: 2026-11-01
- Phase: 1 (affects 2 to 5)
- Made by: Phase 1 owner
- Decision: Accept `GET HEAD POST PUT DELETE OPTIONS PATCH` with HTTP/1.1 or HTTP/1.0 and an absolute `http://` URL (no userinfo, no IPv6 literal, no spaces or control characters; host lower-cased; port default 80; fragment dropped; empty path becomes `/`). Reject with 400: relative targets, `https://` URLs, `CONNECT` and any other method, bad header lines, obsolete line folding, conflicting `Content-Length` values, any `Transfer-Encoding` on requests, headers over `ingress.max_header_bytes` (16384), bodies over `ingress.max_body_bytes` (1 MiB), and requests not completed within `ingress.read_timeout_ms` (5000). The 400 text is always one of our fixed strings and never echoes request bytes.
- Why: The handbook requires 400 for malformed requests and no crashes on garbage. Fixed reason strings avoid log or response injection.
- Alternatives considered: 413 for oversized requests and 501 for unsupported methods (more precise, but the handbook lists only 400 for ingress errors).
- Consequences: `https://` and `CONNECT` through the gateway are not supported yet (I-001).
- Status: Active

## D-008: Status codes the gateway generates
- Date: 2026-11-01
- Phase: 1 (affects 2 to 4)
- Made by: Phase 1 owner
- Decision: 400 malformed, oversized or timed-out request; 502 connect failure, I/O error, unparseable proxy response, or an exception in the handler; 504 connect or response timeout; 503 when the ingress pending queue is full. Phase 2 adds 503 for a full domain queue; 429 and the no-route 502/503 choice are left to Phases 2 and 3. Every gateway-generated reply is `text/plain` with a short fixed message. Replies from a proxy or target are passed through with their own status and carry an extra `X-Gateway-Proxy: <proxy id>` header.
- Why: Matches the error list in handbook section 3.5.
- Alternatives considered: Mapping all errors to 502 (hides the timeout versus failure difference the dashboard needs).
- Consequences: Phase 3 retry logic keys off `ErrorKind`, not the HTTP status.
- Status: Active

## D-009: Configuration format, validation and CLI
- Date: 2026-11-01
- Phase: 1 (affects all)
- Made by: Phase 1 owner
- Decision: One JSON file (`--config FILE`, required) with the keys listed in `docs/INTERFACES.md`. Unknown keys, wrong types and out-of-range values are rejected with a message starting `config:`, printed as `gateway: error: ...` on stderr, exit code 2. All keys of later phases (rate limits, workers, circuit, weights) are accepted and validated already so those phases never need a format change. CLI flags: `--config`, `--policy` (overrides `policy`), `--workers N` (overrides `workers.max`), `--help`. Overrides are validated again.
- Why: UC9 requires clear rejection and a non-zero exit; strict unknown-key rejection catches typos.
- Alternatives considered: Silently ignoring unknown keys (typos would go unnoticed).
- Consequences: Adding a key means updating the key list in `src/common/config.cpp`, `docs/INTERFACES.md` and a test. `--workers` has no effect until Phase 2.
- Status: Active

## D-010: Definition of a successful request
- Date: 2026-11-01
- Phase: 1 (affects 3, 4, 6)
- Made by: Phase 1 owner
- Decision: A request counts in `requests_successful` when the transport worked and the status is below 500, so a 404 from the target is a success. A transport error, or a proxy/target status of 500 or more, counts in `requests_failed` (the 5xx response is still passed through to the client). Timeouts are counted in `timeouts` and also in `requests_failed`. Malformed requests are counted in `bad_requests` and are not part of `requests_total`. `requests_total` counts well-formed requests accepted by ingress.
- Why: A proxy returning 5xx is a proxy failure for health purposes; a 404 is the target working correctly.
- Alternatives considered: Counting any non-2xx as failed (would punish proxies for the target's 404s).
- Consequences: Phase 3 and 4 health feedback should use the same rule. The SRS success definition, if different, must be reconciled (ask the team).
- Status: Active

## D-011: Test-bed nodes are small custom servers, not Squid or tinyproxy
- Date: 2026-11-01
- Phase: 1 (affects 2, 4, 5, 6)
- Made by: Phase 1 owner
- Decision: The target (`testbed/target/server.py`) and the three proxies (`testbed/proxies/faultproxy.py`) are standard-library Python programs. Faults are environment variables (`DELAY_MS`, `FAIL_PCT`) that can also be changed at run time through a per-proxy control port (`GET /fault?delay_ms=..&fail_pct=..`). Proxy 1 normal, Proxy 2 `DELAY_MS=100`, Proxy 3 `FAIL_PCT=20`. The handbook allowed "Squid or tinyproxy as proxies, `tc netem` or a fault option"; this uses the fault option.
- Why: Squid and netem need extra packages and the `NET_ADMIN` capability, and each fault needs a different config. This gives the same three behaviours on any machine, runs without Docker, and is deterministic with `FAIL_SEED`. The proxies speak HTTP/1.1 keep-alive and support CONNECT, so Phase 5 can show connection reuse.
- Alternatives considered: Squid plus `tc netem` (closer to a real proxy but unverifiable here and needs privileges); tinyproxy (no fault option).
- Consequences: Phase 6's "Add 100 ms delay" button can call the control port instead of `tc netem`; the "Kill Proxy 3" button still uses `docker stop proxy3`. Results measured against these proxies reflect a Python proxy, so absolute throughput numbers in Phase 5 and 6 are limited by it; say so in the benchmark report. Swapping in Squid later needs a decision entry.
- Status: Active

## D-012: Test-bed host names and ports
- Date: 2026-11-01
- Phase: 1 (affects 2, 3, 4)
- Made by: Phase 1 owner
- Decision: The target answers to `test.example`, `site-a.example` and `site-b.example` (Docker network aliases on `proxynet`; the target also records requests per Host header at `/stats`). Published on `127.0.0.1` only: target 8080, proxies 3128 / 3129 / 3130, proxy control ports 3228 / 3229 / 3230. Target paths: `/products/<id>`, `/health`, `/stats`, `?delay_ms=N` on any path.
- Why: Phase 2 needs two host names to prove per-domain limits; the handbook example URL is `http://test.example/products/123`.
- Alternatives considered: Separate target containers per site (more moving parts, same result).
- Consequences: The gateway resolves nothing about the target; the proxy does.
- Status: Active

## D-013: A no-Docker local mode exists, and scripts auto-detect Docker
- Date: 2026-11-01
- Phase: 1 (affects all)
- Made by: Phase 1 owner
- Decision: `testbed/run_local.sh start|stop|status` starts the same target and proxies as plain processes (target 8081, same proxy ports). Because there is no Docker network, the proxies get `HOST_OVERRIDES` mapping the `*.example` names to `127.0.0.1:8081`. `scripts/smoke.sh` and `tests/e2e/run_phase1_acceptance.sh` use Docker Compose when a daemon is reachable, otherwise local mode, and accept `--local` to force it.
- Why: Docker was not available where Phase 1 was developed, and teammates without Docker can still work.
- Alternatives considered: Docker only (unverifiable in the development sandbox).
- Consequences: `docker compose up` itself is not yet confirmed (I-002). Phase 4 fault injection needs real `docker stop/start` for realism, but a local kill of a proxy process behaves the same for the gateway.
- Status: Active

## D-014: Gateway container is defined but optional; gateway runs on the host in tests
- Date: 2026-11-01
- Phase: 1 (affects 5, 6)
- Made by: Phase 1 owner
- Decision: `docker-compose.yml` has a `gateway` service under the compose profile `gateway`, so a plain `docker compose up` starts target and proxies only, and `--profile gateway` adds the gateway (image built from `testbed/gateway/Dockerfile`, config `config/gateway.docker.json`, admin published to `127.0.0.1` only). Scripts run the gateway binary on the host with `config/gateway.local.json`, which points at the proxies' published ports on `127.0.0.1`.
- Why: The handbook calls for a gateway container "placeholder". It could not be built or run here, so it must not be able to break the default `docker compose up`.
- Alternatives considered: Gateway always in the default stack (an unverified build step could fail the whole stack).
- Consequences: Whoever first runs it with Docker confirms or fixes the Dockerfile (I-002, I-010) and should then supersede this entry if the profile is dropped.
- Status: Active

## D-015: Admin API shapes fixed in Phase 1
- Date: 2026-11-01
- Phase: 1 (Phase 6 depends on it)
- Made by: Phase 1 owner
- Decision: `GET /health`, `GET /proxies`, `GET /api/stats` (the dashboard stats endpoint) and `GET /metrics` have the field names, types and units in `docs/INTERFACES.md`, served by the stub in `src/admin/` with dummy data plus the real Phase 1 counters. Responses carry `"stub": true` until Phase 6 serves live data. `/ui` serves a placeholder page.
- Why: Handbook task 8; lets Phase 6 build the dashboard against stable names while earlier phases fill in real values.
- Alternatives considered: Leaving shapes to Phase 6 (would force a retrofit of every earlier module's metrics).
- Consequences: Field names may be added but not renamed or removed without a decision entry. Phases 2 to 5 should publish their numbers under the names listed in INTERFACES.md or add new ones and record them there.
- Status: Active

## D-016: Latency percentiles use a bounded ring buffer
- Date: 2026-11-01
- Phase: 1 (Phase 6 may refine)
- Made by: Phase 1 owner
- Decision: `Metrics` keeps the most recent 4096 latency samples in a ring buffer and computes p50, p95 and p99 by nearest rank when asked. Counters are relaxed atomics.
- Why: Bounded memory and a safe default; the request path takes one short mutex.
- Alternatives considered: HDR histogram (more accurate over long runs; Phase 6 task 2 can switch to it behind the same `MetricsSink` interface).
- Consequences: Percentiles describe the last 4096 requests only. The mutex is a possible contention point at 5,000 requests per second; measure in Phase 5 before changing.
- Status: Active

## D-017: Lingering close after an error reply at ingress
- Date: 2026-11-01
- Phase: 1
- Made by: Phase 1 owner (found during the end-to-end acceptance run)
- Decision: After sending a 400 for a malformed or oversized request, the listener half-closes (`shutdown(SHUT_WR)`), then drains incoming bytes for up to 200 ms or 1 MiB before closing.
- Why: Closing a socket that still has unread data makes Linux send a TCP reset right after the reply. HTTP guidance (RFC 9112 section 9.6) recommends a lingering close for exactly this case, because a reset can erase the client's unread input and a client may see "connection reset" instead of the 400. Measured: without it, a 60 KB oversized request got the 400 followed by a reset on 20 of 20 connections; with it, 0 of 20 (unit test "an oversized request gets its 400 and then a clean close"; verified to fail when the fix is removed). Whether the 400 itself is lost depends on timing, so it was not reproduced on loopback.
- Alternatives considered: Reading the whole request first (unbounded work for hostile input).
- Consequences: A malformed request can hold a handler thread for up to 200 ms. Phase 2 (queue-based ingress) must keep this behaviour for 400 replies.
- Status: Active

## D-018: Docker host port for the target moves from 8080 to 18080 and becomes configurable
- Date: 2026-11-01
- Phase: 1
- Made by: Phase 1 owner
- Decision: In `testbed/docker-compose.yml` the target is published on `127.0.0.1:${TARGET_HOST_PORT:-18080}`. This supersedes only the "target 8080" part of D-012; everything else in D-012 stands. `scripts/lib.sh` exports the variable and prints a hint when `docker compose up` fails.
- Why: First real Docker run (on the owner's laptop) failed because host port 8080 was already in use. 8080 is a very common port.
- Alternatives considered: Keeping 8080 (clashes with many dev tools); making the proxy ports configurable too (they are baked into the gateway configs; if 3128 to 3130 clash, change them in the compose file and `config/gateway.local.json` together and log it).
- Consequences: Docs and scripts now say 18080 for Docker mode. Local mode still uses 8081.
- Status: Active

## D-019: Phase 2 queue, release and AIMD design
- Date: 2026-11-01
- Phase: 2
- Made by: Phase 2 owner
- Decision: Ingress transfers a parsed `Job` and its RAII client descriptor to a per-domain FIFO scheduler. New buckets start full (burst B); a scheduler thread wakes at most every 5 ms, refills with the monotonic clock, and releases oldest eligible jobs independently per domain. A fixed `workers.max` thread pool processes released work, gated by AIMD's concurrency limit C. AIMD evaluates one-second windows: an error/timeout rate of 10 percent or more applies `floor(beta*C)` (never below min); otherwise it adds `ceil(alpha)` (never above max).
- Why: Full buckets preserve burst behaviour, one short scheduler tick avoids busy-waiting, and a fixed upper-bound pool proves no request creates a thread. The one-second / 10 percent window reacts within UC6's five-second requirement.
- Alternatives considered: Per-domain scheduler threads; a thread per request; measuring every completion.
- Consequences: A domain queue full condition returns 503 and increments `queue_rejected`. Client descriptors deliberately move from ingress to scheduler, then workers. Phase 3 replaces only the fixed-proxy portion of `GatewayCore`.
- Status: Active

## D-020: Phase 3 routing scores, outcome smoothing and retry ownership
- Date: 2026-11-01
- Phase: 3
- Made by: Phase 3 owner
- Decision: Implement the five configured policies in a `PolicyRouter` behind `Router`. Candidate proxies must be healthy, not OPEN, and not already in `Job::tried_proxies`. Round-robin cycles across the current eligible list; least-connection uses the lowest active count; latency uses the lowest recent RTT; weighted uses `latency`, `load` and `failure` weights over normalized [0,1] values (RTT relative to the eligible-set maximum, active requests relative to the eligible-set maximum, and failure fraction). A dimension with no observed data is neutral (0.5). Ties use the lowest proxy id. Real outcomes update smoothed RTT with EMA alpha 0.2. `GatewayCore` owns retry orchestration: only transport errors are retried, through a different proxy, with at most two retries; target/proxy HTTP responses are returned unchanged. An empty route returns 502.
- Why: This preserves the existing `Router` seam, keeps retries close to request/response mapping, gives comparable scale to policy inputs, and follows the Phase 3 checklist's failed connection/timeout retry behaviour. The eligibility rule deliberately allows HALF_OPEN because Phase 4 controls its one-test gate; OPEN is excluded now.
- Alternatives considered: Put retries in `WorkerPool` (would duplicate HTTP response and metrics mapping); retry HTTP 5xx (would alter the specified pass-through behavior and can duplicate non-idempotent requests); use raw RTT in the weighted score (latency units would dominate arbitrary weights).
- Consequences: At most three attempts per job. Attempts and successful/failed responses update proxy outcome counts; an RAII completion guard keeps active-load accounting balanced on every exit path. Weighted score uses current eligible-set normalization, not a global historical scale.
- Status: Active

## D-021: Preserve the last attempt result when no retry route remains
- Date: 2026-11-01
- Phase: 3
- Made by: Phase 3 owner
- Decision: If a transport failure occurs but the tried-proxy filter leaves no alternate route, return that last attempt's normal 502/504 mapping and `X-Gateway-Proxy`; count the unavailable retry route in `no_route`. Count `retries` and increment `Job::retry_count` only when a different proxy is actually reserved and forwarded.
- Why: A one-proxy configuration must preserve the real timeout status (504) instead of masking it with a later routing condition (502), and retry metrics should represent forwarded retry attempts rather than failed route lookups.
- Alternatives considered: Return 502 for every empty eligible set (hides the prior timeout); count every retry loop as a retry even if no second attempt was sent.
- Consequences: An initially empty route still returns 502 without a proxy header. A failed attempt with no alternate route preserves its error class.
- Status: Active

## D-022: Explore proxies with no RTT sample under latency policy
- Date: 2026-11-01
- Phase: 3
- Made by: Phase 3 owner
- Decision: While the latency policy has eligible proxies with no RTT sample, cycle through those unmeasured proxies before comparing measured RTT values. Once each available proxy has a sample, choose the minimum RTT (ties by proxy id).
- Why: Treating an unknown RTT as infinity avoids selecting it but can permanently starve it; treating it as zero would also select the same lowest-id proxy forever. A bounded round-robin exploration collects the measurements the policy needs.
- Alternatives considered: Treat unknown as zero or infinity; random exploration.
- Consequences: Initial requests under `latency` are distributed to collect samples. Later choices use measured values until eligibility changes and an unmeasured proxy becomes available.
- Status: Active

## D-023: Keep the Phase 2 verifier isolated from Phase 3 routing faults
- Date: 2026-11-01
- Phase: 3
- Made by: Phase 3 owner
- Decision: Update `bench/phase2_verify.py`'s temporary benchmark config to use only the normal first proxy. The script still injects a 100% failure on that proxy during its AIMD test.
- Why: After Phase 3 enables configured routing, the Phase 2 verifier's previous three-proxy pool sent part of its correctness workload through the intentionally failing Proxy 3, causing unrelated 502 responses. The verifier is meant to measure Phase 2 rate isolation and AIMD behavior, not routing.
- Alternatives considered: Disable Proxy 3's fault only, or accept Phase 2 verification as failing after Phase 3. A one-proxy temporary config isolates the exact Phase 2 mechanisms with the fewest dependencies.
- Consequences: This changes only the test script's generated `/tmp` config; production configuration and routing are unaffected.
- Status: Active

## D-024: Phase 4 rolling health, active probes and circuit behavior
- Date: 2026-11-02
- Phase: 4
- Made by: Phase 4 owner
- Decision: Keep a configurable rolling window of 20 combined real-attempt and probe outcomes per proxy. Derive `healthy=false` only after at least four samples when rolling success falls below 50%; otherwise it is healthy. Probe each configured proxy every 2 seconds through the existing `Forwarder` using `GET http://test.example/health`; probe success means a transport success and 2xx response. The circuit opens after the configured consecutive-failure threshold (default 5), stays OPEN for the configured cooldown (default 10 seconds), then allows exactly one monitor probe in HALF_OPEN. The normal router excludes both OPEN and HALF_OPEN. A successful half-open probe closes and resets the breaker; failure reopens it and restarts cooldown. Probe outcomes join per-proxy rolling/lifetime outcomes but update separate global probe counters, not request totals.
- Why: Rolling health prevents one bad sample from immediately disabling a proxy. Four all-failure samples detect the test-bed failure well within the 10-second target at the two-second probe cadence, while the breaker keeps the established 5-failure/10-second policy. Routing traffic must never accidentally consume the half-open test gate.
- Alternatives considered: Marking unhealthy on one failed sample (flaps on transient loss); allowing ordinary requests in HALF_OPEN (cannot guarantee the single-test gate); probing with a separate HTTP client path (would fail to measure the configured proxy forwarding path).
- Consequences: `circuit.window_size` and `circuit.probe_interval_ms` extend the existing config object. Probe URLs rely on the test-bed `test.example` alias and `/health` endpoint. Rolling and lifetime per-proxy counts include probes; global request counters do not. SRS success definition remains unresolved per I-013; D-010 stays unchanged.
- Status: Active

## D-025: Keep health probes out of request attempt counters
- Date: 2026-11-02
- Phase: 4
- Made by: Phase 4 owner
- Decision: Probe outcomes contribute to the combined rolling health/RTT window and the global probe counters, but remain separate from Phase 3's per-proxy request `success_count`, `failure_count` and `requests_sent`. Add per-proxy probe success/failure counters to snapshots and the admin API.
- Why: Preserve the established meaning of Phase 3 request counters and keep request/retry benchmarks stable while still including probes in the health estimate.
- Alternatives considered: Mix probes into the cumulative request counters (would count non-client work as forwarded requests and change Phase 3 verifier results).
- Consequences: `/proxies` and `/metrics` expose distinct traffic and probe counts. `success_rate` remains the rolling combined health rate; request traffic success totals remain available separately.
- Status: Active

## D-026: Bound and isolate per-proxy health probes
- Date: 2026-11-02
- Phase: 4
- Made by: Phase 4 owner
- Decision: Run one fixed monitor thread per configured proxy, with a maximum 1-second connect and response timeout for probes. The normal request path retains its configured deadlines.
- Why: A slow or unresponsive proxy must not delay another proxy's probe cadence; bounded probes allow five consecutive failures to open the circuit within the 10-second detection target at a 2-second cadence.
- Alternatives considered: Probe all proxies sequentially on one thread (one silent proxy could delay every later proxy); use the full request timeout for probes (could exceed the detection target).
- Consequences: Monitor thread count is bounded by configured proxy count; only background probes use the shorter deadline. Forwarding stays outside state locks.
- Status: Active

## D-027: Derive the healthy flag from the most recent four outcomes
- Date: 2026-11-02
- Phase: 4
- Made by: Phase 4 owner
- Decision: Keep the configurable 20-sample rolling window for reported success/RTT statistics, but derive the routing `healthy` flag from the most recent four combined request/probe outcomes. A proxy becomes unhealthy when at least four are available and fewer than half succeeded; it recovers when the latest four reach at least a 50% success rate. If `circuit.window_size` is below four, use the full configured window as the minimum and health window.
- Why: The longer window gives stable visible trend metrics, while a four-sample tail detects a dead proxy within the 10-second target and allows a recovered proxy to become eligible soon after its successful HALF_OPEN probe. A single good probe cannot immediately erase a run of failures.
- Alternatives considered: Use the entire 20-sample window for health eligibility (recovery can remain blocked for many probe intervals after the circuit closes); reset all rolling history on recovery (loses meaningful health history).
- Consequences: `healthy` and the reported `success_rate` use different scopes: `healthy` uses the latest four (or the full smaller configured window); `success_rate` remains the full configured rolling window.
- Status: Active; supersedes D-024's health-flag derivation only

## D-028: Keep the Phase 2 AIMD verifier isolated from Phase 4 health state
- Date: 2026-11-02
- Phase: 4 (minimal verifier update for the Phase 2 check)
- Made by: Phase 4 owner
- Decision: Keep the verifier's one-proxy temporary config and raise its circuit threshold to 100 so its controlled 20-request AIMD fault window does not open the circuit. After fault removal, wait until the health probe has restored eligibility before measuring two healthy AIMD windows.
- Why: Phase 4 correctly makes the only proxy ineligible after the test's injected 502 burst, which prevents the old Phase 2 verifier from measuring recovery of AIMD independently. The check should measure scheduler/AIMD behavior, not circuit cooldown.
- Alternatives considered: Change production health/circuit behavior; accept no-route failures as AIMD recovery; disable health in production configuration. All would weaken the Phase 4 acceptance behavior or the verifier's stated purpose.
- Consequences: The Phase 2 verification script still checks only rate isolation and AIMD, with health recovery allowed to complete before the AIMD success window.
- Status: Active

## D-029: Keep Phase 5 ingress HTTP-only; defer CONNECT tunneling
- Date: 2026-11-02
- Phase: 5
- Made by: Gourav
- Decision: Phase 5 implements non-blocking forward-proxy requests for the existing accepted HTTP methods and absolute `http://` URLs. HTTPS and the CONNECT method remain unsupported at ingress, as defined by D-007.
- Why: Phase 5's performance goal is event-driven forwarding and upstream keep-alive reuse; the team confirmed the existing HTTP-only ingress contract for this phase.
- Alternatives considered: Add CONNECT tunnel parsing and byte-stream tunneling, which would expand the ingress and response model beyond the current `Forwarder` contract.
- Consequences: I-001 is deferred rather than a Phase 5 deliverable. No changes to ingress semantics or the `Forwarder` result interface are required for CONNECT.
- Status: Active

## D-030: Phase 5 epoll reactor and connection-pool bounds
- Date: 2026-11-02
- Phase: 5
- Made by: Codex
- Decision: Use one dedicated level-triggered epoll reactor per `EpollForwarder`. Keep at most 8 upstream sockets per proxy (including active and idle sockets), reuse one idle HTTP/1.1 connection at a time, and evict an idle connection after 30 seconds. All sockets are non-blocking, close-on-exec and set `TCP_NODELAY`.
- Why: Level-triggered readiness simplifies partial read/write handling and stale-socket detection. One fixed reactor can advance many requests without creating a thread per connection. A bounded per-proxy pool keeps the descriptor count finite while allowing parallel requests.
- Alternatives considered: Edge-triggered readiness (requires draining every state transition perfectly); an event loop per request (does not share keep-alive sockets); unbounded pooling (risks descriptor exhaustion).
- Consequences: With at most 16 configured proxies, the forwarder can own at most 128 upstream sockets, in addition to its epoll and wake descriptors. Idle timeout and per-proxy limit are constructor defaults in this phase; configuration keys can be considered later if operational tuning requires them.
- Status: Active

## D-031: Add gauges to the shared metrics sink for upstream pool state
- Date: 2026-11-02
- Phase: 5
- Made by: Codex
- Decision: Extend `MetricsSink` with signed gauge deltas and add Phase 5 counters for opened/reused upstream sockets, timeout classes and upstream bytes. Expose active and idle upstream socket gauges in the existing admin responses.
- Why: Pool state can rise and fall, so counters alone cannot report currently open and idle connections. The metrics must come from the same reactor that owns those descriptors.
- Alternatives considered: Keep private reactor statistics (not visible to `/api/stats` or Prometheus); count every gauge as a monotonic counter (does not describe current state).
- Consequences: Any future `MetricsSink` implementation must implement `add_gauge`; the shared metrics interface and admin schema grow additively.
- Status: Active

## D-032: Make Phase 5 pool bounds configurable with a global cap
- Date: 2026-11-02
- Phase: 5
- Made by: Codex
- Decision: Add `forwarder_pool.max_connections_per_proxy` (default 8), `forwarder_pool.max_total_connections` (default 128), and `forwarder_pool.idle_timeout_ms` (default 30000). Enforce both per-proxy and global socket caps.
- Why: The fixed defaults support normal workloads, while the handbook requires measuring concurrency up to 1,000. Making pool bounds explicit allows controlled load tests without removing descriptor safety.
- Alternatives considered: Raising the defaults (unnecessarily opens many descriptors for ordinary deployments); leave the benchmark target untestable with a fixed eight-per-proxy limit.
- Consequences: Strict config parsing validates each bound and requires the global cap to be at least the per-proxy cap. Total sockets remain capped at 16,384 by config validation.
- Status: Active

## D-033: Report GatewayCore overhead separately from forwarder time
- Date: 2026-11-02
- Phase: 5
- Made by: Codex
- Decision: Record a bounded 4,096-sample percentile window for total `GatewayCore::handle` duration minus the summed time spent in its forwarder attempts. Expose p50, p95 and p99 in `/api/stats` and `/metrics`.
- Why: The Phase 5 acceptance target measures gateway processing overhead excluding proxy and target time; forwarder time already measures those attempts.
- Alternatives considered: Infer overhead by subtracting separately measured client percentiles (measurement noise and non-paired samples); add timing to ingress and reply writes (broader end-to-end instrumentation, beyond the current core target).
- Consequences: The reported value covers gateway-core processing around forwarding, including routing/retry bookkeeping, and excludes request queue delay and client write time.
- Status: Active

## D-034: Disable Nagle on the test proxy sockets used by Phase 5 benchmarks
- Date: 2026-11-02
- Phase: 5
- Made by: Codex
- Decision: Set `TCP_NODELAY` on accepted sockets in the Python fault proxy so small HTTP response writes are not delayed on persistent gateway-to-proxy connections.
- Why: A direct 30-request keep-alive sample against the original test proxy measured about 42 ms p50 and 43 ms p95, while per-connection requests were about 1–2 ms. That showed the proxy's Nagle/delayed-ACK interaction was dominating the keep-alive comparison rather than gateway overhead.
- Alternatives considered: Attribute the measured delay to the gateway (the same delay appears when bypassing it); discard keep-alive measurements from the standard test bed.
- Consequences: Phase 1–4 correctness behavior and fault settings are unchanged; Phase 5 performance measurements against the persistent test-proxy path become representative of gateway reuse.
- Status: Active

## D-035: Evict the oldest idle socket when the global pool cap is reached
- Date: 2026-11-02
- Phase: 5
- Made by: Codex
- Decision: If a request needs a new connection while the total pool cap is full, close the oldest idle socket across all proxies and use the freed slot. Never evict a socket with an in-flight request.
- Why: A busy proxy must be able to acquire capacity when another proxy has unused idle sockets; otherwise the global cap can strand queued requests until an unrelated idle timeout expires.
- Alternatives considered: Wait for idle timeout (needlessly delays requests); evict any active socket (would interrupt a request).
- Consequences: An idle proxy connection may be reopened on its next request when the gateway is under global descriptor pressure.
- Status: Active

## D-036: Keep request-result output off the forwarding path
- Date: 2026-11-02
- Phase: 6
- Made by: Codex
- Decision: Use a bounded 4,096-row producer queue and a dedicated writer thread for CSV or newline-delimited JSON output. Keep the latest 50 redacted request summaries in memory for the dashboard. Redact URL credentials, path and query before either view or file output; count dropped output rows.
- Why: Disk latency must not slow forwarding, and the handbook prohibits credentials or response bodies in logs.
- Alternatives considered: Synchronous per-request file writes; retaining full URLs or response bodies.
- Consequences: If the writer queue fills or disk writes fail, forwarding continues and `output_rows_dropped` reports loss. JSON output is JSON Lines (one object per row).
- Status: Active

## D-037: Apply dashboard policy and default rate changes at runtime
- Date: 2026-11-02
- Phase: 6
- Made by: Codex
- Decision: Expose an allow-listed admin settings route that switches among the five existing routing policies and updates the default domain token-bucket rate and burst under the scheduler lock. Explicit per-domain overrides remain in force.
- Why: The dashboard controls must change the running gateway while preserving the fixed routing and limiter interfaces.
- Alternatives considered: Restart the gateway for each setting change; overwrite per-domain limits along with the default.
- Consequences: The control endpoint is mutating and is exposed only on the admin listener, localhost-bound by default. Policy changes take effect for subsequent routing decisions.
- Status: Active

## D-038: Keep demo fault controls local and allow-listed
- Date: 2026-11-02
- Phase: 6
- Made by: Codex
- Decision: Run a separate test-bed control service on `127.0.0.1:9100`. It accepts only fixed kill, delay, restore and benchmark actions; the gateway itself does not execute Docker or shell commands.
- Why: Demonstration controls must be separate from gateway request handling and must not expose arbitrary process control.
- Alternatives considered: Letting the gateway execute shell commands; binding control actions on the public ingress interface.
- Consequences: Local mode uses the test-bed PID file and proxy fault-control ports. Docker mode uses only the fixed container name `proxy3`; browser origins are limited to the local dashboard.
- Status: Active

## D-039: Add a browser dashboard as an operator surface
- Date: 2026-11-02
- Phase: 6
- Made by: Codex
- Decision: Add the static `/ui` dashboard as an operator layer over the existing admin/metrics API and test-bed controls. The dashboard does not replace CLI/configuration workflows or change the gateway request protocol.
- Why: Phase 6 requires live monitoring and demonstration controls, while the supplied Design Document v1.0 describes operator configuration plus metrics/log outputs and CSV/JSON measurements but no browser UI. The SRS source is not present in this checkout.
- Alternatives considered: Edit the supplied PDF directly (no editable source is available here); change the gateway's HTTP interface to require the dashboard.
- Consequences: Before final release, update the Design Document source and SRS with the dashboard, local-only admin/control surfaces, charts, controls and benchmark results. Current spec updates are recorded as implementation notes only.
- Status: Active

## D-040: Compare health-aware and health-unaware selection without bypassing circuits
- Date: 2026-11-02
- Phase: 6
- Made by: Codex
- Decision: Add a runtime dashboard/benchmark toggle for the rolling `healthy` eligibility flag. OPEN and HALF_OPEN circuit states remain excluded in both modes.
- Why: The benchmark suite needs to measure health-aware routing under injected proxy faults while preserving the Phase 4 circuit-breaker safety contract.
- Alternatives considered: Send requests through OPEN circuits (would invalidate Phase 4 behavior); omit the health-awareness comparison.
- Consequences: `health_aware=false` can send traffic to a proxy marked unhealthy if its circuit remains CLOSED. `/api/stats` reports the current mode.
- Status: Active

## D-041: Expose live test-proxy fault settings as bounded dashboard controls
- Date: 2026-11-03
- Phase: 6
- Made by: Codex
- Decision: Let the dashboard set delay (0 to 5,000 ms) and injected failure rate (0 to 100%) for the three fixed test-bed proxies, and provide baseline, delayed-proxy, flaky-proxy and combined presets. The gateway's upstream host/port configuration stays visible but read-only in this form.
- Why: These controlled fault settings exercise routing, health and circuit recovery without restarting the gateway or editing its deployment config.
- Alternatives considered: Let browser input select arbitrary process commands or target arbitrary proxy-control ports; mutate the gateway's live proxy endpoint set, which would also require connection-pool and health-monitor lifecycle changes.
- Consequences: The control service accepts only the three named test proxies and fixed numeric bounds. Per-proxy fault changes apply immediately; opening circuits and recovery remain governed by Phase 4.
- Status: Active

## D-042: Use a synthetic PriceLens offer-monitoring story for the demo
- Date: 2026-11-03
- Phase: 6
- Made by: Codex
- Decision: Demonstrate gateway behavior as PriceLens checking five SKU offers across three fictional retailer hostnames. The local target returns stable synthetic prices/availability; the load generator sends those checks through the gateway and the dashboard shows the latest observations.
- Why: A concrete product-monitoring workflow makes proxy routing, per-domain limits, latency, failures, and recovery legible in a live presentation without depending on external shopping sites.
- Alternatives considered: Generate generic requests with no business context; scrape real storefronts, which adds external availability, policy, and data-quality dependencies.
- Consequences: The dashboard and demo guide label prices as local synthetic data. No real retailer data is fetched. Third-party proxy integration remains separately scoped and is not implied by this demo.
- Status: Active

## D-043: Expand the demo pool to five independently configurable proxies
- Date: 2026-11-03
- Phase: 6
- Made by: Codex
- Decision: Add local Proxy 4 (250 ms delay) and Proxy 5 (40% injected failures), with unique gateway/control ports, per-proxy dashboard editing, a latency ladder, a reliability gradient, and a combined preset. Keep per-proxy delay and failure rate bounded and editable.
- Why: A larger pool with varied behavior makes routing, health scoring, circuit breaking, and recovery easier to demonstrate than a three-node pool.
- Alternatives considered: Keep three proxies and change only their values; add provider-backed endpoints, which would require a selected provider and protocol/auth support.
- Consequences: The Phase 6 local test bed binds ports 3131–3132 and 3231–3232 in addition to its existing proxy ports. Phase 1–5 verification scripts retain their three-proxy assumptions; the Docker gateway example now lists all five nodes.
- Status: Active

## D-044: Add optional Basic client authentication and exact egress allowlists
- Date: 2026-11-03
- Phase: Productization extension
- Made by: Codex
- Decision: Add optional HTTP Basic client authentication, sourcing the password from a named environment variable, and an optional exact hostname/port allowlist. Enable both in the loopback-only PriceLens demo. Do not accept secret values in JSON configuration.
- Why: Client access and destination policy are prerequisites for a reusable outbound gateway; the existing local demo remains bounded to its fictional retailer targets.
- Alternatives considered: Keep unrestricted requests and rely only on bind address; put credentials directly in tracked config; add wildcard host rules.
- Consequences: Rejected credentials receive 407 and disallowed host/port pairs receive 403. Existing configs remain backward-compatible when security options are absent. Basic auth is plaintext without TLS, and hostname allowlists do not pin DNS results; see I-017 and I-018.
- Status: Active

## D-045: Offer TLS-wrapped gateway access through an optional Compose profile
- Date: 2026-11-03
- Phase: Productization extension
- Made by: Codex
- Decision: Add an optional `tls` Compose profile using stunnel to wrap the gateway's TCP listener. Require externally supplied certificate/key files for deployment, generate a self-signed localhost certificate only in the local demo script, use TLS 1.2 or later, and bind to loopback by default.
- Why: Protect Basic client credentials on client-to-gateway connections without changing the gateway's existing socket/forwarder implementation in this slice.
- Alternatives considered: Implement TLS throughout the C++ ingress and worker write path; expose plaintext gateway access beyond loopback.
- Consequences: The TLS profile protects client-to-gateway transport only. Raw port 8000 remains loopback-published; TLS does not provide HTTPS CONNECT to destination sites. The image built and local handshake passed on 2026-11-03; production deployment still requires a trusted certificate and deliberate bind address.
- Status: Active


## D-046: Reduce Phase 5 hot-path contention and size the test backlog for concurrency validation
- Date: 2026-11-03
- Phase: 5 follow-up
- Made by: Codex
- Decision: Store bounded latency samples in independently indexed per-slot rings, allow concurrent policy reads with a shared mutex, use an atomic round-robin cursor, and keep proxy snapshots/reservations outside the policy replacement lock. Raise the gateway and local Python test-server listen backlogs to 2,048 for the controlled 1,000-client trial.
- Why: The original global metric and selection locks serialized requests during the Phase 5 high-concurrency benchmark; the previous 512-entry listen backlogs also constrained the controlled test.
- Alternatives considered: Accept the 718-active-worker ceiling and measured lock contention; replace the existing worker/forwarder interfaces with a new asynchronous scheduler.
- Consequences: Policy replacement remains exclusive and safe; normal request selection can proceed concurrently. The bounded sample window remains 4,096 observations. Current benchmark reaches 1,000 active workers with 3.29 ms p95 GatewayCore overhead. The 5,000 requests/s target remains below the Python test-bed ceiling, and raw packet capture remains an environment blocker.
- Status: Active
