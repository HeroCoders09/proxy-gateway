# Intelligent Concurrent Proxy Gateway: Team Handbook and Phase Plan

Nov 1, 2026 · @Gourav

## 1. Project context (read this first)

The Intelligent Concurrent Proxy Gateway is a C++20/Linux networking system that takes a high volume of HTTP requests and forwards each one to a target server through the best proxy in a managed pool. It is the Group 1 project for the Computer Networks course at IIT Roorkee, and it is built in six serial phases, one owner per phase.

### 1.1 The problem it solves

Simple rotating-proxy scrapers pick proxies at random, keep sending traffic to dead or slow proxies, open a new connection for every request, and either under-use the network or overload the target. Our dummy customer, PriceLens Analytics, collects 50,000 product pages per hour from 5 retailers through 8 rented proxies and reports four pains:

- 10 to 15 percent of requests fail because dead proxies keep being selected.
- Traffic bursts get them blocked by retailers.
- Throughput collapses when one proxy turns slow.
- They cannot see which proxy or site is causing failures.

Each pain maps to a gateway mechanism, and each mechanism is a classic networking topic: congestion control, load balancing, fault tolerance, connection management and traffic shaping.

### 1.2 How a request flows

Every request follows one path: **Schedule, Control, Select, Connect, Forward, Measure.**

1. **Ingress.** The gateway listens on TCP port 8000 and accepts proxy-style HTTP/1.1 requests with absolute URLs. A malformed request gets HTTP 400.
2. **Schedule.** The request becomes a Job in a per-domain FIFO queue (limit 10,000 jobs; overflow returns 503).
3. **Control.** A per-domain token bucket (refill rate r, burst B) releases jobs. A bounded worker pool, whose size is adapted by an AIMD rule, picks up released jobs. No thread is created per request.
4. **Select.** The Proxy Manager removes unhealthy proxies and proxies whose circuit is OPEN, then applies a routing policy: Random, Round Robin, Least Connections, Lowest Latency or Weighted Score.
5. **Connect.** The Connection Manager reuses an idle keep-alive connection to that proxy or opens a new non-blocking one.
6. **Forward.** The Request Forwarder sends the request through the proxy using epoll and returns the response to the client. Timeouts are 2 s to connect and 10 s for the response.
7. **Measure.** Latency, status, proxy and success are recorded for the metrics and the CSV/JSON output.

If an attempt fails, the job is retried through a different proxy, up to 2 times. After 5 consecutive failures a proxy's circuit opens and it is skipped. After a 10 s cooldown, one test request decides whether it returns to service.

### 1.3 What we are building and what we are not

**In scope (core):** scheduler and rate limiter, bounded worker pool with AIMD, proxy manager with all five policies, health monitor, circuit breaker, connection reuse over epoll, metrics, benchmarking, and a web dashboard for demonstration.

**Out of scope:** large machine-learning models, anti-bot evasion, dependence on public proxy lists, a commercial scraping platform, and microsecond or high-frequency-trading performance claims.

**Optional extensions (only after Phase 6 works):** learned proxy selection, kernel and socket tuning, a minimal SOCKS5 proxy node, Grafana. A proxy-health prediction model is deferred for now.

### 1.4 Targets we will be measured against

| Area | Target |
| --- | --- |
| Gateway overhead | 5 ms or less at p95, excluding proxy and target time |
| Throughput | At least 5,000 requests per second against the local test server |
| Concurrency | At least 1,000 in-flight requests |
| Reliability | With 1 of 3 proxies failing, success rate of 99 percent or more; failed proxy detected within 10 s |
| Memory | 256 MB or less at 1,000 concurrent requests; no file-descriptor leaks |
| Scale | 2 to 16 proxies, at least 20 domains |
| Security | Admin and metrics endpoints bind to localhost; no credentials or response bodies in logs |

### 1.5 Source documents

The authoritative specs are the **Requirements and Specification Document v1.0** (use case IDs UC1 to UC12, functional requirements FR-1 to FR-18) and the **Design Document v1.0**. When this handbook and those documents disagree, record the conflict in `docs/DECISIONS.md` and ask the team before choosing. One known deviation: the SRS says the system has no GUI of its own, but we add a web dashboard to make demonstration easy, and that change must be written into the next revision of both documents.

## 2. Working protocol (applies to every person and every AI agent)

Work happens in strict order: Phase 1, then 2, then 3, up to 6. Only one person works at a time, and each phase starts from the working system the previous owner left. The project must build, pass its tests and run its demo command at the end of every phase. The next person must be able to continue without asking anyone, so **everything that matters is written down in the repository, not kept in someone's head or in a chat.**

### 2.1 The tracking folder

Phase 1 creates a `docs/` folder at the repository root. Every later owner keeps it current.

| File | Purpose | Who writes it |
| --- | --- | --- |
| `docs/PROJECT_CONTEXT.md` | A short copy of Section 1 of this handbook, so an agent working in the repo can read the project background without this document | Phase 1 owner, then fixed only by agreement |
| `docs/DECISIONS.md` | Every decision taken: what was chosen, why, what else was considered, and which phase it affects. Append-only | Every owner, as decisions are made |
| `docs/PROGRESS.md` | A running log of what was done and what is next, updated during the work. The first place a new owner or agent reads to find where work stopped | Every owner and every agent, continuously |
| `docs/HANDOFF_PHASE_N.md` | One file per finished phase: what exists, how to run it, how to test it, known problems, and what the next owner should do first | The phase owner, when finishing |
| `docs/INTERFACES.md` | The shared structs, function signatures, config keys and admin API JSON shapes. Changes need a decision entry | Phase 1 owner creates; later owners update when they change an interface |
| `docs/ISSUES.md` | Bugs, gaps and deferred work that are not fixed yet, with the phase that should fix them | Everyone |

Templates for each file are in Section 10.

### 2.2 Rules for every person and every AI agent

1. **Start by reading.** Before changing anything, read in this order: `docs/PROJECT_CONTEXT.md`, the latest `docs/HANDOFF_PHASE_N.md`, `docs/PROGRESS.md` (bottom entries first), `docs/DECISIONS.md`, `docs/INTERFACES.md`, `docs/ISSUES.md`. Then run the build and the test suite to confirm the starting point is green.
2. **Log decisions as you make them.** Any choice another person could have made differently goes into `docs/DECISIONS.md` right away: a library, a data structure, an algorithm parameter, a naming rule, a change to an interface, a deviation from the SRS or design document. Record what was decided, why, the alternatives rejected, and the phase affected. Never rewrite an old entry. To reverse a decision, add a new entry that supersedes it.
3. **Write progress as you work.** After each meaningful step (a module compiles, a test passes, a bug is found) append a dated line to `docs/PROGRESS.md`. Before stopping for any reason, including running out of time or context, add an entry saying exactly what is done, what is half done, what is next, and the command that shows the current state. An agent that is cut off mid-task must leave the file in a state where the next agent can resume from it.
4. **Do not edit earlier phases' code without cause.** If a bug or missing feature in an earlier phase blocks you, make the smallest fix, add a decision entry naming the file and the reason, and note it in `docs/ISSUES.md` or `docs/PROGRESS.md`. Never silently rewrite someone else's module.
5. **Respect the interfaces.** Code against the headers in `docs/INTERFACES.md` and `src/common/`. Changing a shared struct or API shape needs a decision entry and an update to `docs/INTERFACES.md` in the same commit.
6. **Keep the system runnable.** Every commit builds. Every phase ends with passing unit tests, a working demo command, and updated docs. Do not leave a phase with failing tests and a promise to fix them later.
7. **Expose your own numbers.** Each phase adds the counters and gauges for its own module to the shared `Metrics` struct (see each phase), so the dashboard in Phase 6 is wiring, not retrofitting.
8. **Write tests with the code.** The SRS requires unit tests for the scheduler, rate limiter and routing. Add tests for every module you own and record how to run them in your handoff.
9. **Separate facts from guesses.** In the docs, mark anything not verified as `UNVERIFIED`. Do not claim a target (for example 5,000 requests per second) is met without a recorded measurement.
10. **Commit discipline.** One branch per phase, named `phase-N-short-name`, merged to `main` only when the phase's done checklist is complete. Commit messages say what changed and mention the decision ID when one applies, for example `Add token bucket (D-007)`.
11. **Never log secrets or response bodies.** This is an SRS security requirement and applies to debug output as well.
12. **Ask when blocked.** If something is ambiguous or the documents conflict, write the question in `docs/PROGRESS.md` under `OPEN QUESTIONS`, ask the team, and record the answer as a decision.

### 2.3 Instructions to paste when handing a phase to an AI agent

> You are continuing the Intelligent Concurrent Proxy Gateway project. Work only on Phase N as described in the team handbook. First read, in order: `docs/PROJECT_CONTEXT.md`, the latest `docs/HANDOFF_PHASE_*.md`, `docs/PROGRESS.md`, `docs/DECISIONS.md`, `docs/INTERFACES.md` and `docs/ISSUES.md`. Run the build and tests to confirm they pass before you change anything. While you work, append decisions to `docs/DECISIONS.md` and dated progress lines to `docs/PROGRESS.md`. If you must stop, record exactly what is done, what is half done, what is next and the command to see current state. When the phase's done checklist is complete, write `docs/HANDOFF_PHASE_N.md`. Do not modify earlier phases' code except for minimal fixes recorded as decisions.

### 2.4 Handoff ritual (end of every phase)

1. Run the full build and unit tests from a clean checkout.
2. Run the phase's demo command and confirm the expected result.
3. Write `docs/HANDOFF_PHASE_N.md` from the template.
4. Update `docs/INTERFACES.md`, `docs/ISSUES.md` and `docs/PROGRESS.md`.
5. Merge the branch to `main` and tag it `phase-N-done`.
6. Tell the next owner which file to open first: the handoff file.

## 3. Architecture, stack and shared conventions

The gateway is one C++ program made of modules that talk through small, documented structs. Around it sit a Docker test bed, a web dashboard and a tiny control service. Phase 1 fixes everything in this section in `docs/INTERFACES.md`; later phases may extend it but must log changes as decisions.

### 3.1 Technology stack

| Layer | Choice |
| --- | --- |
| Language and build | C++20, CMake 3.20 or newer, GCC 11+ or Clang 14+, Ubuntu 22.04+ |
| Networking | POSIX sockets, non-blocking I/O, epoll, HTTP/1.1 keep-alive, HTTP CONNECT and forward proxying |
| Concurrency | Bounded worker pool with `std::thread`, mutexes held briefly, `std::atomic` for counters, RAII for every file descriptor |
| Config and JSON | nlohmann/json (header only) |
| Unit tests | GoogleTest or Catch2 (pick one in Phase 1 and log it) |
| Test bed | Docker Compose: Nginx or small custom server as target, Squid or tinyproxy as proxies, `tc netem` for delay and loss |
| Load generation | `wrk` plus a small custom C++ or Python generator controllable from the dashboard |
| Dashboard | One static HTML page, vanilla JavaScript and Chart.js, served by the gateway's admin port |
| Control service | About 50 lines of Python (Flask) or Node, runs in the test bed, executes `docker stop/start` and `tc netem` for the dashboard buttons |
| Verification | tcpdump or Wireshark for handshake and keep-alive evidence |

### 3.2 Repository layout

```text
proxy-gateway/
  CMakeLists.txt
  config/gateway.json          example configuration
  src/
    common/                    Job, ProxyState, Connection, Metrics, GatewayConfig, error types
    ingress/                   HTTP listener and request parser (Phase 1)
    scheduler/                 domain queues, token bucket (Phase 2)
    workers/                   worker pool, AIMD controller (Phase 2)
    routing/                   proxy table, eligibility, policies, retry (Phase 3)
    health/                    probes, rolling stats, circuit breaker (Phase 4)
    net/                       forwarder interface, simple forwarder (Phase 1), epoll forwarder and connection pool (Phase 5)
    admin/                     admin HTTP server, metrics endpoint, static UI hosting (Phase 6)
    output/                    CSV and JSON result writer (Phase 6)
    main.cpp
  tests/                       unit tests, one folder per module
  testbed/
    docker-compose.yml
    target/  proxies/  control-service/
  dashboard/                   index.html, app.js, styles.css
  bench/                       benchmark scripts and results/
  docs/                        PROJECT_CONTEXT, DECISIONS, PROGRESS, HANDOFF_PHASE_N, INTERFACES, ISSUES
  README.md
```

### 3.3 Shared data structures

Defined in `src/common/` in Phase 1. Fields may be added later but not renamed or removed without a decision entry.

| Struct | Main fields |
| --- | --- |
| `Job` | id, url, domain, method, headers, body, enqueue time, retry count, tried-proxy list, client connection handle |
| `ProxyState` | id, host, port, healthy flag, recent RTT, success and failure counts, consecutive failures, active connections, last probe time, circuit state (CLOSED, OPEN, HALF\_OPEN) |
| `DomainQueue` | FIFO of jobs, queue limit, tokens, refill rate, capacity, last refill time |
| `Connection` | fd (RAII wrapper), proxy id, state (IDLE, CONNECTING, ACTIVE, CLOSED), keep-alive metadata, pending buffers |
| `Metrics` | counters (requests total, successful, failed, timeouts), gauges (active connections, queue length per domain, worker count, current AIMD limit), per-proxy stats, latency samples for p50, p95, p99 |
| `GatewayConfig` | proxy list, routing policy, per-domain rate and burst, worker min, max, alpha, beta, timeouts, circuit threshold and cooldown, output settings, ports |

### 3.4 Fixed interfaces between modules

| Interface | Contract |
| --- | --- |
| `Forwarder` | `forward(job, proxy) -> Result` with status, body, latency and error kind. The simple forwarder (Phase 1) and the epoll forwarder (Phase 5) both implement it, so Phase 5 swaps the implementation without touching other code |
| `Router` | `select(job, proxy_table) -> optional<ProxyId>`; returns nothing when no route is available |
| `RateLimiter` | `try_acquire(domain) -> bool`, using a monotonic clock that tests can fake |
| `HealthReporter` | `report(proxy_id, success, rtt)` called after every attempt and every probe |
| `MetricsSink` | Thread-safe increment and observe functions; the only way modules write measurements |

### 3.5 Ports and endpoints

| Port | Service | Notes |
| --- | --- | --- |
| 8000 | Gateway client port | Proxy-style HTTP/1.1 with absolute URLs. Errors: 400, 429, 502, 503, 504 |
| 9000 | Admin and dashboard | Bound to localhost. `/metrics` (Prometheus text), `/health` (JSON), `/proxies` (JSON), `/ui` (dashboard) |
| 9100 | Control service | Test bed only. Used by dashboard buttons for kill, delay and restore |

Phase 1 must write the exact JSON field names for `/health`, `/proxies` and any extra dashboard endpoint (for example `/api/stats`) into `docs/INTERFACES.md` and keep them stable. Phase 6 depends on them.

### 3.6 Coding conventions

- Every socket and file descriptor lives in an RAII wrapper. No raw `close()` calls scattered in code.
- Never hold a lock while doing blocking network I/O.
- Use a monotonic clock for all timing, behind a small clock interface so tests can control time.
- Buffers, queues and connection pools have explicit upper bounds.
- Handle peer resets and malformed input without crashing.
- Configuration is external JSON or CLI flags. Changing a policy never requires recompiling.
- Compile with `-Wall -Wextra` and run at least one sanitizer build (`-fsanitize=address,undefined`) before each handoff.

## 4. Phase 1: Foundation and skeleton

**Owner:** Person 1 (name: \_\_\_\_\_\_). **Branch:** `phase-1-foundation`. **Covers:** UC1, UC9, FR-1 (intake only), FR-11 (status and body return), part of FR-5.

**Goal:** a request travels client, gateway, one proxy, target and back, on a repository and test bed that every later phase builds on.

### 4.1 Inputs

- The SRS v1.0 and Design Document v1.0.
- This handbook, Sections 1 to 3.
- An empty repository.

### 4.2 Tasks

1. **Repository and build.** Create the layout in Section 3.2, a CMake project with C++20, warning flags, a sanitizer build option, and the chosen unit-test framework wired in with one passing placeholder test.
2. **Tracking folder.** Create all six files in `docs/` from the templates in Section 10. Fill `PROJECT_CONTEXT.md` from Section 1. Log the first decisions (test framework, JSON library, branch rules, port numbers).
3. **Docker Compose test bed** on a network named `proxynet`:
   - a controlled target server (Nginx or small custom server) with `/products/<id>` pages, a `/health` endpoint for probes, and an optional configurable response delay;
   - three proxy nodes (Squid or tinyproxy): **Proxy 1 normal, Proxy 2 with +100 ms delay, Proxy 3 with about 20 percent failures** (using `tc netem` or a fault option);
   - a placeholder for the gateway container.
4. **Shared structs and interfaces.** Write `Job`, `ProxyState`, `DomainQueue`, `Connection`, `Metrics`, `GatewayConfig` and the interfaces in Section 3.4 in `src/common/`. Add an RAII file-descriptor wrapper and a clock interface.
5. **Configuration.** Load `gateway.json` and CLI flags (`--config`, `--policy`, `--workers`, `--help`). Validate everything. An invalid config is rejected with a clear error and a non-zero exit code.
6. **Ingress.** Listen on port 8000. Parse proxy-style HTTP/1.1 requests with absolute URLs. Return **400** for malformed requests. Never crash on garbage input or peer resets.
7. **Simple forwarder.** Implement the `Forwarder` interface with plain blocking sockets to a single configured proxy, with connect and response timeouts. Return the target's status and body to the client. Phase 5 will replace this.
8. **Interface contract for the dashboard.** Define and document the JSON shapes of `/health`, `/proxies` and the stats endpoint in `docs/INTERFACES.md` (fields, types, units). Add a stub admin server that returns those shapes with dummy data.
9. **Smoke test script.** `scripts/smoke.sh` starts the test bed and the gateway, sends a request through the gateway and checks the status and body.

### 4.3 Deliverables

| Deliverable | Where |
| --- | --- |
| Building CMake project with sanitizer option and a unit-test framework | repo root, `tests/` |
| Docker Compose test bed with target and three proxies (normal, +100 ms, 20 percent failures) | `testbed/` |
| Shared structs, interfaces, RAII wrapper, clock interface | `src/common/` |
| Config loader with validation and CLI flags | `src/common/config*` |
| Ingress listener and parser returning 400 on bad input | `src/ingress/` |
| Simple single-proxy forwarder | `src/net/` |
| Documented admin API JSON shapes and a stub server | `docs/INTERFACES.md`, `src/admin/` |
| Six tracking files, filled | `docs/` |
| `docs/HANDOFF_PHASE_1.md` | `docs/` |

### 4.4 Metrics this phase adds

`requests_total`, `requests_successful`, `requests_failed`, `timeouts` in the shared `Metrics` struct.

### 4.5 Done checklist

- [ ] `GET http://test.example/products/123` through port 8000 returns the correct status and body (UC1).
- [ ] 1,000 sequential requests return correct bodies.
- [ ] A malformed request returns HTTP 400 and the gateway keeps running.
- [ ] An invalid config file is rejected with a clear message and a non-zero exit code (UC9).
- [ ] `docker compose up` starts the full test bed with one command.
- [ ] Delay on Proxy 2 and failures on Proxy 3 are verified with direct requests.
- [ ] Sanitizer build passes the unit tests with no errors.
- [ ] `docs/` files exist, decisions logged, handoff written.

## 5. Phase 2: Scheduling and concurrency

**Owner:** Person 2 (name: \_\_\_\_\_\_). **Branch:** `phase-2-scheduling`. **Covers:** UC3, UC6, FR-1 (queues and 503), FR-2, FR-3, FR-4.

**Goal:** requests are queued per domain, released at a controlled rate, and processed by a bounded worker pool whose size adapts to network conditions.

### 5.1 Inputs

- `docs/HANDOFF_PHASE_1.md` and the working Phase 1 system.
- The shared structs and interfaces in `src/common/` (`Job`, `DomainQueue`, `RateLimiter`, `Forwarder`, `MetricsSink`, clock interface).
- Config keys for rate limits and workers (`rate`, `burst`, `min`, `max`, `alpha`, `beta`) defined in Phase 1.
- The test bed with a target server on at least two hostnames, so that site A and site B can be limited differently.

### 5.2 Tasks

1. **Domain queues.** One bounded FIFO queue per target domain, created on first use. Default limit 10,000 jobs. When full, the request is answered immediately with **HTTP 503**. Keep critical sections short.
2. **Token bucket.** Per domain: refill rate r, capacity B. On each check, add tokens from elapsed time (capped at B). One token releases the oldest queued job; with no token the job stays queued. Use the clock interface so tests can control time.
3. **Release loop.** A scheduler thread (or timer) that releases ready jobs to the worker pool without busy-waiting. Domains must be independent: a limited site A must not slow site B.
4. **Bounded worker pool.** A fixed set of threads, up to `max`, that take ready jobs, call the router and forwarder, write the response back to the client and report the outcome. No thread per request.
5. **AIMD controller.** Maintain concurrency limit C between `min` and `max`. When the recent window is healthy, C increases by alpha. When timeout or error pressure is detected, C becomes the floor of beta times C (0 < beta < 1). Define the window length and the error threshold, and log them as decisions. Workers only run while active jobs are below C.
6. **Temporary router stub.** Until Phase 3, the workers call a trivial router that returns the single configured proxy. Keep it behind the `Router` interface so Phase 3 replaces it.
7. **Unit tests** for the token bucket (burst, refill, cap, per-domain isolation), queue overflow, worker pool bounds and AIMD increase and decrease, all driven by the fake clock.
8. **Load test.** A script that sends requests to two domains at high rate and records the arrival rate at the target for each.

### 5.3 Deliverables

| Deliverable | Where |
| --- | --- |
| Per-domain bounded queues with 503 on overflow | `src/scheduler/` |
| Token-bucket rate limiter | `src/scheduler/` |
| Bounded worker pool | `src/workers/` |
| AIMD concurrency controller with documented parameters | `src/workers/` |
| Unit tests for scheduler, rate limiter and AIMD | `tests/scheduler/`, `tests/workers/` |
| Rate-limit load-test script and recorded result | `bench/` |
| `docs/HANDOFF_PHASE_2.md` | `docs/` |

### 5.4 Metrics this phase adds

Queue length per domain, rejected-503 count, tokens available per domain, worker count (threads), current AIMD limit C, active jobs.

### 5.5 Done checklist

- [ ] Configuring 50 requests per second for site A gives no more than 50 per second at the target, while site B is unaffected (UC3).
- [ ] A full queue returns 503 and the gateway stays healthy.
- [ ] The process never creates more threads than the configured maximum under load.
- [ ] After injecting 100 ms delay or failures, C drops within 5 seconds and recovers afterwards (UC6). Record the graph or numbers.
- [ ] Unit tests pass, including under the sanitizer build.
- [ ] Decisions logged: AIMD window and thresholds, alpha and beta defaults, queue and release design.
- [ ] Handoff written.

## 6. Phase 3: Routing and retries

**Owner:** Person 3 (name: \_\_\_\_\_\_). **Branch:** `phase-3-routing`. **Covers:** UC2, UC4 (retry part), FR-5, FR-6, FR-9.

**Goal:** every job is sent through a proxy chosen by a selectable policy, and a failed attempt is retried through a different proxy.

### 6.1 Inputs

- `docs/HANDOFF_PHASE_2.md` and the working Phase 2 system.
- The `Router` interface, `ProxyState` struct and the temporary single-proxy router stub from Phase 2.
- The three test-bed proxies with different behaviour (normal, slow, failing).
- The worker pool's outcome reporting, which feeds success, failure and RTT back to the proxy table.

### 6.2 Tasks

1. **Proxy state table.** Build a thread-safe in-memory table of `ProxyState` records created from the config. Provide short-lock accessors and snapshot reads, so a routing decision uses a consistent copy.
2. **Eligibility filter.** Exclude proxies that are unhealthy or whose circuit state is OPEN. In this phase the health flag and circuit state are fields that default to healthy and CLOSED, and Phase 4 will drive them. Write the filter now so that Phase 4 only has to set the fields. Also exclude proxies already tried by this job.
3. **Routing policies**, all behind the `Router` interface and selectable at start-up with `--policy` or the config:

   | Policy | Rule |
   | --- | --- |
   | `random` | Uniform choice among eligible proxies |
   | `roundrobin` | Cycle through eligible proxies; the fairness baseline |
   | `leastconn` | Smallest active-connection count |
   | `latency` | Smallest recent RTT |
   | `weighted` | Minimum of S = wL times L + wC times C + wF times F, using normalized latency, load and failure penalty. Put the weights in config |
4. **Active-load accounting.** Increment a proxy's active-connection count when it is selected and decrement it on completion, including on failure paths. Verify no leaks.
5. **No-route handling.** If the eligible set is empty, return no route; the gateway answers 502 or 503 and counts it.
6. **Outcome feedback.** After every attempt, update the proxy's success or failure counts, consecutive failures and an RTT average (for example an exponentially weighted moving average). Expose this through the `HealthReporter` interface so Phase 4 can extend it.
7. **Retry logic.** On a timeout or connection error, mark the attempt failed, add the proxy to the job's tried list and retry through a different eligible proxy, at most 2 retries, then report the failure to the client. Count retries.
8. **Unit tests** for every policy (including a case with one proxy ineligible), the eligibility filter, the active-count accounting and the retry limit.
9. **Policy comparison script** that runs the same load under each policy and records p50, p95, p99 latency, throughput and success rate.

### 6.3 Deliverables

| Deliverable | Where |
| --- | --- |
| Thread-safe proxy state table | `src/routing/` |
| Eligibility filter | `src/routing/` |
| Five routing policies behind `Router`, selected from config or CLI | `src/routing/` |
| Active-load accounting | `src/routing/` |
| Retry through a different proxy, maximum 2 | `src/workers/` or `src/routing/` (log the choice) |
| Unit tests | `tests/routing/` |
| Policy comparison script and recorded results | `bench/` |
| `docs/HANDOFF_PHASE_3.md` | `docs/` |

### 6.4 Metrics this phase adds

Per proxy: requests sent, successes, failures, active connections, average RTT. Global: retries, no-route count, selections per policy.

### 6.5 Done checklist

- [ ] The policy can be switched at start-up without recompiling.
- [ ] With a proxy forced ineligible, 0 requests are assigned to it (UC2).
- [ ] With least-connections, active counts across proxies stay within plus or minus 2 (UC2).
- [ ] A request that times out on Proxy 3 succeeds through another proxy, with at most 2 retries (UC4, retry part).
- [ ] Policy comparison results recorded, with a short note on which policy wins under which condition.
- [ ] Unit tests pass, including under the sanitizer build.
- [ ] Decisions logged: weight defaults, RTT smoothing factor, where retry logic lives.
- [ ] Handoff written.

## 7. Phase 4: Health and fault tolerance

**Owner:** Person 4 (name: \_\_\_\_\_\_). **Branch:** `phase-4-health`. **Covers:** UC4, FR-7, FR-8, completes FR-5.

**Goal:** the gateway notices failing proxies on its own, stops using them, and brings them back when they recover.

### 7.1 Inputs

- `docs/HANDOFF_PHASE_3.md` and the working Phase 3 system.
- The proxy state table, eligibility filter and `HealthReporter` interface from Phase 3.
- A `/health` endpoint on the test-bed target for probes.
- The ability to stop and restart a proxy container (`docker stop` and `docker start`).

### 7.2 Tasks

1. **Health monitor thread.** Probe every proxy at a fixed period (default **2 seconds**) by requesting the controlled `/health` endpoint through that proxy. Record success or failure and the RTT. Probes must never block request processing, and must run through the same code path the real traffic uses where possible, so a probe reflects real behaviour.
2. **Rolling statistics.** Keep, per proxy, the last N RTT samples, a success rate over the last N attempts (probes and real requests), and the number of consecutive failures. N is configurable (log the default as a decision).
3. **Health flag.** Derive `healthy` from the rolling summary, not from a single sample. Define the rule (for example, success rate below a threshold, or probes failing consecutively) and record it.
4. **Circuit breaker per proxy.** Implement the state machine:
   - **CLOSED:** normal use. **5 consecutive failures** move it to OPEN.
   - **OPEN:** the proxy is skipped by the eligibility filter. After a **10 second cooldown** it becomes HALF\_OPEN.
   - **HALF\_OPEN:** exactly one test request (or probe) is allowed. Success returns it to CLOSED and resets the failure count. Failure returns it to OPEN and restarts the cooldown.

   Make the threshold and cooldown configurable. Only one test may be in flight in HALF\_OPEN.
5. **Feed from both sources.** Real request outcomes and probe outcomes both update failure counts and the circuit. Retried attempts count against the proxy that failed.
6. **Wire into routing.** The Phase 3 eligibility filter reads `healthy` and the circuit state. Confirm no OPEN proxy receives normal traffic.
7. **Thread safety.** The breaker is touched by workers and the monitor. Use short critical sections or atomics, and never hold a lock during network I/O.
8. **Unit tests** for every circuit transition (including HALF\_OPEN failure and success, and a single test in flight), the rolling statistics, and health derivation, with the fake clock for cooldown.
9. **Fault-injection scripts** that kill a proxy mid-run, restart it, and add or remove delay, recording success rate and time to detect and recover.

### 7.3 Deliverables

| Deliverable | Where |
| --- | --- |
| Health monitor with 2 s probes | `src/health/` |
| Rolling RTT and success statistics per proxy | `src/health/` |
| Circuit breaker state machine | `src/health/` |
| Integration with the eligibility filter | `src/routing/` |
| Unit tests | `tests/health/` |
| Fault-injection scripts and recorded results | `bench/`, `testbed/` |
| `docs/HANDOFF_PHASE_4.md` | `docs/` |

### 7.4 Metrics this phase adds

Per proxy: circuit state, consecutive failures, rolling success rate, last probe time and RTT, number of times the circuit has opened. Global: probe successes and failures.

### 7.5 Done checklist

- [ ] With Proxy 3 killed, 0 requests are assigned to it after its circuit opens (UC2).
- [ ] A failed proxy is detected within 10 seconds.
- [ ] With one of three proxies killed mid-run, at least 99 percent of requests succeed (UC4).
- [ ] After the proxy is restarted, it returns to use after the cooldown through HALF\_OPEN to CLOSED.
- [ ] Circuit state is visible through `/proxies` (stub or real) with the documented field names.
- [ ] Unit tests pass, including under the sanitizer build.
- [ ] Decisions logged: rolling window size, health rule, probe design, how probe and real outcomes are combined.
- [ ] Handoff written.

## 8. Phase 5: Fast network I/O

**Owner:** Person 5 (name: \_\_\_\_\_\_). **Branch:** `phase-5-epoll-io`. **Covers:** UC5, FR-10, FR-11 (timeouts), non-functional performance and resource targets.

**Goal:** replace the simple blocking forwarder with an event-driven, non-blocking forwarder that reuses keep-alive connections, without changing any other module's code.

### 8.1 Inputs

- `docs/HANDOFF_PHASE_4.md` and the working Phase 4 system.
- The `Forwarder` interface and `Connection` struct from `src/common/`.
- The simple forwarder in `src/net/`, kept as a correctness reference and fallback.
- tcpdump or Wireshark available in the test bed, plus `wrk` or the custom load generator.

### 8.2 Tasks

1. **RAII socket layer.** Wrap sockets and the epoll descriptor in RAII types. Set non-blocking mode and `TCP_NODELAY`. Guarantee that no descriptor leaks on any error path.
2. **Event loop.** One or a small fixed number of epoll loops that wait for ready events and advance connection state machines. Do not dedicate a blocked thread to each connection. Choose edge-triggered or level-triggered epoll and log the choice with the reason.
3. **Connection state machine.** Implement `IDLE, CONNECTING, ACTIVE, CLOSED` with transitions: new request leads to connect attempt, socket event makes it usable, completed request returns it to IDLE, any I/O failure closes it.
4. **Per-proxy connection pool.** Keep a bounded set of idle keep-alive connections per proxy. On request: reuse a valid idle connection, otherwise open a new non-blocking one. Detect and discard stale connections (closed by peer, idle too long). Cap the pool size and the total descriptor count.
5. **HTTP/1.1 forwarding.** Send the request through the proxy using forward-proxy style or HTTP CONNECT as configured. Parse the response status line, headers and body, handling `Content-Length` and chunked transfer encoding. Respect `Connection: keep-alive` and `close`. Bound all buffers.
6. **Timeouts.** Enforce a connect timeout (default 2 s) and a response timeout (default 10 s) per attempt using timer handling in the event loop. A timeout produces the same error kind the Phase 3 retry logic already understands.
7. **Interface fit.** Implement the existing `Forwarder` interface and select it from config (for example `forwarder: epoll`), keeping the simple forwarder available. The integration must not require edits in the scheduler, routing or health code. If it does, log the reason as a decision.
8. **Robustness.** Survive peer resets, half-closed connections, partial reads and writes, and slow proxies without crashing or leaking.
9. **Measurement.** Measure and record: gateway overhead at p95 excluding proxy and target time, throughput against the local test server, maximum concurrent in-flight requests, and memory at 1,000 concurrent requests. Capture a tcpdump trace that shows connection reuse.
10. **Unit and integration tests** for the connection state machine, pool reuse and eviction, response parsing edge cases, and timeout behaviour.

### 8.3 Deliverables

| Deliverable | Where |
| --- | --- |
| RAII socket and epoll wrappers | `src/net/` |
| Epoll event loop | `src/net/` |
| Connection state machine and per-proxy keep-alive pool | `src/net/` |
| Non-blocking HTTP/1.1 forwarder behind the existing interface | `src/net/` |
| Connect and response timeouts | `src/net/` |
| Tests | `tests/net/` |
| Measurement report: overhead, throughput, in-flight, memory | `bench/results/` |
| tcpdump trace or screenshot showing reuse, plus a keep-alive versus connection-per-request latency comparison | `bench/results/` |
| `docs/HANDOFF_PHASE_5.md` | `docs/` |

### 8.4 Metrics this phase adds

Active connections, idle pooled connections per proxy, connections opened, connections reused, reuse ratio, connect-timeout and response-timeout counts, bytes in and out.

### 8.5 Done checklist

- [ ] tcpdump shows one SYN, SYN-ACK, ACK sequence serving many requests to the same proxy (UC5).
- [ ] Keep-alive latency is lower than connection-per-request, with numbers recorded.
- [ ] Gateway overhead is 5 ms or less at p95, measured and recorded.
- [ ] Throughput target of 5,000 requests per second and 1,000 in-flight requests measured. Record the real numbers, even if the target is missed, and note the bottleneck in `docs/ISSUES.md`.
- [ ] Memory is 256 MB or less at 1,000 concurrent requests, and a descriptor-leak check shows none (for example compare open fds before and after a long run).
- [ ] All Phase 1 to 4 acceptance tests still pass with the new forwarder.
- [ ] Sanitizer build passes, with address and undefined behaviour checks.
- [ ] Decisions logged: epoll mode, pool limits, idle timeout, parsing rules, number of loop threads.
- [ ] Handoff written.

## 9. Phase 6: Dashboard, metrics, benchmarks and demo

**Owner:** Person 6 (name: \_\_\_\_\_\_). **Branch:** `phase-6-dashboard`. **Covers:** UC7, UC8, FR-12, FR-13, plus the web GUI. Ideally the person with the most frontend experience.

**Goal:** make the whole system observable, controllable and demonstrable from a browser, and produce the benchmark results that prove each mechanism works.

### 9.1 Inputs

- `docs/HANDOFF_PHASE_5.md` and the working Phase 5 system.
- The `Metrics` struct, now holding counters from all five earlier phases.
- The admin API JSON shapes fixed in `docs/INTERFACES.md` since Phase 1.
- The test bed, the load generator, and the per-phase benchmark scripts in `bench/`.

### 9.2 Tasks

1. **Real admin API.** Replace the Phase 1 stub with live data on port 9000, bound to localhost: `GET /metrics` (Prometheus text), `GET /health` (JSON), `GET /proxies` (JSON with state, RTT and circuit state for each proxy), and the dashboard stats endpoint. Keep the field names from `docs/INTERFACES.md`. Metrics to expose: `requests_total`, `requests_successful`, `requests_failed`, `timeouts`, `active_connections`, `requests_per_second`, `proxy_rtt`, `proxy_success_rate`, `proxy_failure_count`, `worker_count`, `queue_length`, and latency percentiles p50, p95, p99.
2. **Percentile computation.** Compute p50, p95 and p99 from bounded latency samples (histogram or ring buffer) without blocking the request path.
3. **Result output.** Write every completed request as a CSV or JSON row: timestamp, url, status, proxy, latency in ms, success. Use a bounded buffer and a background writer so output never slows forwarding. Never write response bodies.
4. **Dashboard (static page, `dashboard/`).** Served at `/ui`. Polls the admin endpoints about once per second. Panels:
   - proxy table: health, RTT, active connections, success rate, circuit state coloured green, yellow or red;
   - live charts: requests per second, p50, p95, p99 latency, success rate;
   - queue length per domain;
   - AIMD concurrency value over time;
   - scrolling log of recent requests showing which proxy handled each.
5. **Control panel in the dashboard.** Routing-policy dropdown (restart or hot switch; log which), rate-limit settings, start and stop load generator, and the test-bed buttons **Kill Proxy 3**, **Add 100 ms delay**, **Restore**. Also a **Run benchmark** button that shows the result table.
6. **Control service** (`testbed/control-service/`). A small service on port 9100 that runs `docker stop/start` and `tc netem` commands. The gateway itself never touches Docker. Lock it to localhost and to the test-bed containers only.
7. **Benchmark suite.** One command reproduces the results table (UC8) as CSV with p50, p95, p99, throughput and success rate for each experiment:
   - routing policies compared;
   - proxy pool size (for example 2, 4, 8, 16);
   - fixed concurrency versus AIMD;
   - keep-alive on versus off;
   - health-aware routing on versus off, under fault injection.

   Include a short interpretation for each result, and say plainly where a result is inconclusive.
8. **Metrics accuracy check.** Compare gateway counters with load-generator totals and confirm they match within 1 percent.
9. **Demo script and rehearsal.** Write the demo steps from Section 10.1 into `docs/DEMO.md` with exact commands and what to click. Rehearse it at least twice from a clean start, and record anything that can go wrong with its recovery step.
10. **Update the specification documents.** Record the GUI as a change to the SRS and Design Document, since both currently say there is no GUI of their own.

### 9.3 Deliverables

| Deliverable | Where |
| --- | --- |
| Live admin API with all required metrics | `src/admin/` |
| Percentile computation | `src/common/` or `src/admin/` |
| CSV and JSON result writer | `src/output/` |
| Web dashboard with monitoring and control panels | `dashboard/` |
| Control service | `testbed/control-service/` |
| One-command benchmark suite with results table and interpretation | `bench/`, `bench/results/` |
| Metrics accuracy check result | `bench/results/` |
| `docs/DEMO.md` and a rehearsal log | `docs/` |
| Notes for updating the SRS and Design Document | `docs/DECISIONS.md` |
| `docs/HANDOFF_PHASE_6.md` (final project handoff) | `docs/` |

### 9.4 Done checklist

- [ ] `/metrics` counters match load-generator totals within 1 percent (UC7).
- [ ] The dashboard updates live and every panel shows real data.
- [ ] Kill, delay and restore buttons work and the dashboard reacts as expected.
- [ ] One command produces the full benchmark table as CSV (UC8).
- [ ] Result rows are written to CSV or JSON, with no response bodies and no credentials in any log.
- [ ] Admin and metrics ports are bound to localhost by default.
- [ ] The demo has been rehearsed twice from a clean start.
- [ ] Decisions logged: percentile method, dashboard endpoints, polling interval, policy switch method.
- [ ] Final handoff written, with the full run instructions.

## 10. Demo, extensions, status tracker and file templates

### 10.1 Demo story for the final evaluation

1. Start the test bed and gateway with one command, open the dashboard, and start the load generator. The dashboard shows steady requests per second and latency.
2. Click **Kill Proxy 3**. Its circuit turns red, traffic moves to the other proxies, and success rate stays above 99 percent.
3. Click **Restore**. The circuit goes yellow (HALF\_OPEN) after the cooldown, then green.
4. Click **Add 100 ms delay**. The AIMD concurrency line drops, then recovers when the delay is removed.
5. Switch the policy from Round Robin to Weighted Score and show the change in p95 latency.
6. Show the rate limit holding for one domain while another is unaffected.
7. Show a tcpdump capture proving many requests over one connection.
8. Click **Run benchmark** and show the results table.

### 10.2 Optional extensions (only after Phase 6 is done and signed off)

| Extension | Reference | Note |
| --- | --- | --- |
| Learned proxy selection per domain | UC11, FR-14 to FR-16 | May only choose among eligible proxies, must not bypass the circuit breaker, and must fall back to the weighted policy |
| Kernel and socket tuning profile | UC12, FR-17, FR-18 | Reversible, with a default-versus-tuned benchmark covering latency, throughput and CPU cost |
| Minimal SOCKS5 proxy node | UC10 | Greeting, connect request and relay |
| Grafana dashboard | Section 4.9 of the SRS | Scrapes `/metrics` through Prometheus |
| Proxy health prediction | Deferred by the team | Revisit only if time remains; would feed a failure probability into the weighted score |

Any extension needs a decision entry before work starts.

### 10.3 Phase status tracker

The current owner updates this table when a phase starts and finishes. Mirror it in `docs/PROGRESS.md`.

| Phase | Name | Owner | Status | Handoff file |
| --- | --- | --- | --- | --- |
| 1 | Foundation and skeleton |  | Not started | `docs/HANDOFF_PHASE_1.md` |
| 2 | Scheduling and concurrency |  | Not started | `docs/HANDOFF_PHASE_2.md` |
| 3 | Routing and retries |  | Not started | `docs/HANDOFF_PHASE_3.md` |
| 4 | Health and fault tolerance |  | Not started | `docs/HANDOFF_PHASE_4.md` |
| 5 | Fast network I/O |  | Not started | `docs/HANDOFF_PHASE_5.md` |
| 6 | Dashboard, metrics, benchmarks, demo |  | Not started | `docs/HANDOFF_PHASE_6.md` |

### 10.4 Template: `docs/DECISIONS.md`

Append-only. Number decisions D-001, D-002 and so on. Never edit an old entry; supersede it with a new one.

```markdown
# Decisions log

## D-001: <short title>
- Date: YYYY-MM-DD
- Phase: N (and any other phase affected)
- Made by: <person or agent name>
- Decision: <what was chosen, in one or two sentences>
- Why: <the reason>
- Alternatives considered: <what else, and why it was rejected>
- Consequences: <what later phases must know or do>
- Status: Active | Superseded by D-0XX
```

### 10.5 Template: `docs/PROGRESS.md`

Newest entries at the bottom. Add a line after each meaningful step, and a full stop entry before you stop for any reason.

```markdown
# Progress log

## Current state (keep this block up to date)
- Current phase: N
- Current owner: <name>
- Branch: <branch>
- Last green build and test run: YYYY-MM-DD HH:MM
- Command that shows current state: <command>

## Open questions
- <question>, asked to <who>, on <date>

## Log
- YYYY-MM-DD HH:MM <name or agent>: <what was done>
- YYYY-MM-DD HH:MM <name or agent>: <what was done>

## Stop entry (write this before stopping)
- Done: <list>
- Half done: <what, which file, what remains>
- Next steps, in order: <1, 2, 3>
- Known problems: <list>
- How to resume: <exact commands>
```

### 10.6 Template: `docs/HANDOFF_PHASE_N.md`

```markdown
# Handoff: Phase N <name>

- Owner: <name>   Dates: <start> to <end>   Branch/tag: <phase-N-done>

## What now exists
<modules and files built, with one line each>

## How to build, run and test
<exact commands from a clean checkout>

## Demo command and expected result
<command, and what should be seen>

## Acceptance results
<each item from the done checklist, with the measured result or evidence>

## Interfaces added or changed
<link to the INTERFACES.md sections>

## Decisions made
<list of decision IDs>

## Known problems and shortcuts
<list, and the matching entries in ISSUES.md>

## First things the next owner should do
1. <step>
2. <step>
```

### 10.7 Template: `docs/INTERFACES.md` and `docs/ISSUES.md`

```markdown
# Interfaces

## Structs
<Job, ProxyState, DomainQueue, Connection, Metrics, GatewayConfig: fields and meaning>

## Module interfaces
<Forwarder, Router, RateLimiter, HealthReporter, MetricsSink: signatures and contracts>

## Config file keys
<key, type, default, meaning>

## Admin API
<endpoint, method, response JSON with field names, types, units>

## Change history
- YYYY-MM-DD D-0XX: <what changed and why>
```

```markdown
# Issues

## I-001: <short title>
- Found in phase: N   Should be fixed in phase: M
- Description: <what is wrong or missing>
- Impact: <what it affects>
- Status: Open | Fixed in <commit> | Won't fix (reason)
```
