# Issues

Bugs, gaps and deferred work that are not fixed yet, with the phase that should fix them.

## I-001: Only absolute `http://` requests are accepted; `https://` and CONNECT return 400
- Found in phase: 1   Should be fixed in phase: 5 (decide first)
- Description: Ingress rejects `https://` URLs and the `CONNECT` method (D-007). A real client sending HTTPS through the gateway would need a CONNECT tunnel.
- Impact: The handbook scope is HTTP/1.1 forward proxying with absolute URLs, so this is intentional for now. The test-bed proxies already support CONNECT.
- Status: Open (ask the team whether CONNECT is required before Phase 5 starts)

## I-002: `docker compose up` and the Docker test bed are not verified
- Found in phase: 1   Should be fixed in phase: 1 (first action of whoever has Docker), otherwise 2
- Description: No Docker daemon was available in the Phase 1 development environment. The compose file parses as valid YAML and the same servers were verified in local mode (`testbed/run_local.sh`), but `docker compose -f testbed/docker-compose.yml up -d --build --wait` has not been run. UNVERIFIED: image builds, the `proxynet` aliases (`test.example` and the others), health checks, published ports.
- Impact: Phase 1 checklist item "`docker compose up` starts the full test bed with one command" is not confirmed, so `phase-1-done` has not been tagged (D-003).
- Status: Fixed 2026-11-01. Verified on the owner's laptop with Docker: all four containers Healthy about 4 s after `up`, `scripts/smoke.sh` passed in Docker mode, and `testbed/verify_testbed.sh` passed against the containers (proxy2 minus proxy1 100.9 ms; proxy3 17.5 percent failures of 200; proxy1 0 percent). First attempt failed on a host port clash (8080), fixed in D-018. The harmless warning "Docker Compose is configured to build using Bake, but buildx isn't installed" appears and can be ignored.

## I-003: Ingress is synchronous (one handler thread per in-flight request)
- Found in phase: 1   Should be fixed in phase: 2
- Description: A handler thread parses the request, forwards it and replies (D-005). In-flight requests are capped at `ingress.threads` (16) and a slow proxy holds a thread for up to the response timeout.
- Impact: No queues, rate limits or AIMD yet. This is the Phase 2 starting point.
- Status: Fixed 2026-11-01 in Phase 2: ingress now parses then transfers the client descriptor to the domain scheduler; bounded workers forward and write replies. The fixed ingress parsing pool remains by design.

## I-004: Client connections are closed after every response
- Found in phase: 1   Should be fixed in phase: 5 or 6
- Description: Every reply carries `Connection: close`; there is no client-side keep-alive.
- Impact: Load generators that reuse connections (`wrk`) reconnect for each request, which adds client-side cost to throughput measurements. Does not affect correctness.
- Status: Open

## I-005: Routing policy is read but not applied
- Found in phase: 1   Should be fixed in phase: 3
- Description: Previously `policy` and `--policy` were validated but ignored by `GatewayCore`. Phase 3 applies all five policies and the `weights` values. Phase 4 now updates health and circuit state from real attempts and active probes, with the router excluding unhealthy, OPEN and HALF_OPEN proxies.
- Impact: Resolved for routing, health and circuit eligibility.
- Status: Fixed 2026-11-02 in Phase 4.

## I-006: Admin server is a single-threaded stub with dummy data
- Found in phase: 1   Should be fixed in phase: 6
- Description: `src/admin/` answers one request at a time and returns dummy values for everything except the Phase 1 counters. `/ui` is a placeholder.
- Impact: None before Phase 6; field names are fixed in INTERFACES.md.
- Status: Open

## I-007: Test-bed proxies and target are Python, so absolute throughput is limited by them
- Found in phase: 1   Should be fixed in phase: 5 (report), 6
- Description: D-011. The 5,000 requests per second target is measured against the "local test server"; a Python target and Python proxies may cap the result before the gateway does.
- Impact: Risk of misattributing a bottleneck. Phase 5 should measure the target alone (for example with `wrk` straight at it) and say which component limits the result; consider an nginx target in Docker if the Python target is the limit.
- Status: Open

## I-008: Blocking `getaddrinfo` per request in the Phase 1 forwarder
- Found in phase: 1   Should be fixed in phase: 5
- Description: The simple forwarder resolves the proxy host name for every request. With IP literals (the local config) this is instant; with Docker names it hits the resolver each time.
- Impact: Small latency cost in Phase 1 to 4; avoid in the epoll forwarder (resolve once per proxy, cache).
- Status: Open

## I-009: Test-bed verification thresholds are statistical
- Found in phase: 1   Should be fixed in phase: any
- Description: `testbed/verify_testbed.sh` samples 200 requests and accepts a Proxy 3 failure rate of 10 to 30 percent (observed 14.5 to 21 percent). A rare unlucky sample could fall outside the band. Setting `FAIL_SEED` makes the proxy's failure pattern reproducible if this ever flakes.
- Impact: Possible rare false failure of the acceptance script.
- Status: Open (low priority)

## I-010: Gateway Dockerfile and GCC 11 / Ubuntu 22.04 build are not verified
- Found in phase: 1   Should be fixed in phase: 1 or 2
- Description: The code was built and tested with GCC 13.3 and Clang 18 on Ubuntu 24.04. The handbook targets GCC 11+ / Ubuntu 22.04+, and `testbed/gateway/Dockerfile` (Ubuntu 22.04) has never been built. UNVERIFIED.
- Impact: Possible compile error on older compilers; the gateway container is optional (D-014).
- Status: Partly fixed 2026-11-01: a GCC 11.4.0 build on Ubuntu 22.04 (owner's laptop) compiled with no warnings and passed all 71 test cases. Still open: `testbed/gateway/Dockerfile` has never been built (`docker compose --profile gateway up --build`).

## I-011: Target `/stats` keeps one counter per second forever
- Found in phase: 1   Should be fixed in phase: 2
- Description: `testbed/target/server.py` records per-second request counts per host in memory without pruning.
- Impact: Negligible for short runs; grows slowly during long benchmarks. Prune or window it when the Phase 2 load test uses it.
- Status: Open

## I-012: Oversized requests get 400, not 413
- Found in phase: 1   Should be fixed in phase: won't fix unless the team decides otherwise
- Description: D-007 chose 400 for every ingress error to match the handbook's error list.
- Impact: Less precise error for clients.
- Status: Won't fix (see D-007)

## I-013: SRS success definition has not been reconciled with D-010
- Found in phase: 1   Should be fixed in phase: 4 (before final acceptance)
- Description: D-010 counts transport success with a status below 500 as a successful request. The authoritative SRS text is not present in this checkout, so it remains UNVERIFIED whether that matches the SRS definition.
- Impact: Phase 3 routing and proxy health use D-010 consistently; a differing SRS definition may require a later decision and metric adjustment.
- Status: Open; bring the SRS text to the next team sync.
