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
