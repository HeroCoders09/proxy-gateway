# Issues

Bugs, gaps and deferred work that are not fixed yet, with the phase that should fix them.

## I-001: Only absolute `http://` requests are accepted; `https://` and CONNECT return 400
- Found in phase: 1   Should be fixed in phase: 5 (decide first)
- Description: Ingress rejects `https://` URLs and the `CONNECT` method (D-007). A real client sending HTTPS through the gateway would need a CONNECT tunnel.
- Impact: The handbook scope is HTTP/1.1 forward proxying with absolute URLs, so this is intentional for now. The test-bed proxies already support CONNECT.
- Status: Deferred by D-029 (Phase 5 remains HTTP-only; revisit only if the team later requires HTTPS tunneling)

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
- Status: Phase 6 replaces stub data and `/ui` placeholder with live endpoints and dashboard. The admin server remains single-threaded; runtime validation is pending.

## I-007: Test-bed proxies and target are Python, so absolute throughput is limited by them
- Found in phase: 1   Should be fixed in phase: 5 (report), 6
- Description: D-011. The 5,000 requests per second target is measured against the "local test server"; a Python target and Python proxies may cap the result before the gateway does.
- Impact: Risk of misattributing a bottleneck. Phase 5 direct measurements show the Python proxy is the limiter; throughput remains below target.
- Status: Measured 2026-11-03; in the final Phase 5 run direct Python target reached 4,427/s, direct proxy 2,196/s, and epoll gateway 1,684/s at concurrency 32. The 5,000/s goal remains missed on this test-bed path. See `bench/results/phase5_performance.md`; consider a native target/proxy for isolating gateway capacity.

## I-008: Blocking `getaddrinfo` per request in the Phase 1 forwarder
- Found in phase: 1   Should be fixed in phase: 5
- Description: The simple forwarder resolves the proxy host name for every request. With IP literals (the local config) this is instant; with Docker names it hits the resolver each time.
- Impact: Small latency cost in Phase 1 to 4; avoid in the epoll forwarder (resolve once per proxy, cache).
- Status: Fixed in Phase 5 for `EpollForwarder`: proxy addresses are resolved and cached on first use. `SimpleForwarder` retains its reference behavior.

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

## I-014: Raw loopback packet capture is unavailable in the current environment
- Found in phase: 5   Should be fixed in phase: 5 (run on a host with capture capability)
- Description: The final Phase 5 benchmark ran with local-network access but `tcpdump -i lo` still exited with `Operation not permitted`; this runtime has no `CAP_NET_RAW`/`CAP_NET_ADMIN`.
- Impact: The required packet-level SYN/keep-alive trace could not be saved. `bench/results/phase5_reuse_syscalls.txt` provides supplemental syscall evidence, and the latest benchmark recorded 32 opened plus 3,468 reused upstream connections.
- Status: Open environment blocker; rerun the benchmark on a host with `CAP_NET_RAW` or `CAP_NET_ADMIN` to create `bench/results/phase5_keepalive.pcap`.

## I-015: Phase 6 runtime acceptance and demo rehearsals are pending
- Found in phase: 6   Should be fixed in phase: 6 (before sign-off)
- Description: The current Phase 6 implementation has not yet been validated with the unit/sanitizer suites, a dashboard runtime walkthrough, benchmark counter-accuracy review, or two clean-start demo rehearsals.
- Impact: The dashboard and control paths may still have integration issues; the implementation is not ready to claim Phase 6 acceptance.
- Status: Fixed 2026-11-03. Normal and sanitizer suites, local dashboard/control walkthrough, Docker control actions, 16-scenario benchmark (0% request-counter delta), output privacy check, and two clean-start rehearsals are recorded in `docs/HANDOFF_PHASE_6.md`.

## I-016: Provider support is limited to HTTP forward proxies
- Found in phase: 6   Should be fixed in phase: future provider integration
- Description: Proxy entries support Basic upstream authentication through an environment variable. Dashboard endpoint display remains read-only and fault controls remain specific to the bundled local test bed. Client HTTPS CONNECT and TLS to upstream providers are outside the current protocol contract.
- Impact: HTTP-only provider endpoints can be configured, including providers requiring Basic auth. Credentials are not encrypted between gateway and upstream proxy on the current plain HTTP transport.
- Status: Partly fixed 2026-11-03; provider-specific protocol/TLS support remains future work. Never store provider credentials in tracked config or docs.

## I-017: Egress hostname allowlists do not pin or validate resolved IP addresses
- Found in phase: productization   Should be fixed in phase: productization before internet-facing deployment
- Description: The new destination policy matches the parsed hostname and port exactly, but the gateway delegates destination DNS resolution to the selected upstream proxy. It cannot currently reject a permitted name that resolves to a private or otherwise restricted address.
- Impact: An allowlisted hostname with unsafe DNS results could still reach an unintended network through a capable upstream proxy. Keep the service on loopback/trusted networks until destination IP policy is enforced at the egress point.
- Status: Open; tracked as a security requirement for the next productization slice.

## I-018: Client Basic credentials are sent over plaintext gateway HTTP
- Found in phase: productization   Should be fixed in phase: productization before remote client access
- Description: The gateway listener itself does not provide TLS. Basic proxy credentials are only encoded, not encrypted, on its client connection. An optional Docker Compose `tls` profile now wraps the gateway listener with stunnel.
- Impact: Credentials can be observed if clients use the raw HTTP port on an untrusted network. The TLS front end is not yet build/runtime-verified; keep its default bind on 127.0.0.1 until configured with a trusted certificate and verified.
- Status: Local TLS front end built and verified with a self-signed certificate on 2026-11-03. Remote access still requires a trusted certificate and correct bind address; keep this open for production deployment readiness.
