# Project context

Short copy of Section 1 of the team handbook ("Intelligent Concurrent Proxy Gateway: Team Handbook and Phase Plan"), so anyone working in this repository can read the background without the original document. Fixed by agreement only: change it only with a decision entry in `docs/DECISIONS.md`.

## 1. What this is

The Intelligent Concurrent Proxy Gateway is a C++20/Linux networking system that takes a high volume of HTTP requests and forwards each one to a target server through the best proxy in a managed pool. It is the Group 1 project for the Computer Networks course at IIT Roorkee, built in six serial phases, one owner per phase.

### 1.1 The problem it solves

Simple rotating-proxy scrapers pick proxies at random, keep sending traffic to dead or slow proxies, open a new connection for every request, and either under-use the network or overload the target. Our dummy customer, PriceLens Analytics, collects 50,000 product pages per hour from 5 retailers through 8 rented proxies and reports four pains:

- 10 to 15 percent of requests fail because dead proxies keep being selected.
- Traffic bursts get them blocked by retailers.
- Throughput collapses when one proxy turns slow.
- They cannot see which proxy or site is causing failures.

Each pain maps to a gateway mechanism, and each mechanism is a classic networking topic: congestion control, load balancing, fault tolerance, connection management and traffic shaping.

### 1.2 How a request flows

Every request follows one path: **Schedule, Control, Select, Connect, Forward, Measure.**

1. **Ingress.** Listen on TCP port 8000; accept proxy-style HTTP/1.1 requests with absolute URLs. Malformed request: HTTP 400.
2. **Schedule.** The request becomes a Job in a per-domain FIFO queue (limit 10,000 jobs; overflow returns 503).
3. **Control.** A per-domain token bucket (refill rate r, burst B) releases jobs. A bounded worker pool, sized by an AIMD rule, picks up released jobs. No thread per request.
4. **Select.** The Proxy Manager removes unhealthy proxies and proxies whose circuit is OPEN, then applies a routing policy: Random, Round Robin, Least Connections, Lowest Latency or Weighted Score.
5. **Connect.** The Connection Manager reuses an idle keep-alive connection to that proxy or opens a new non-blocking one.
6. **Forward.** The Request Forwarder sends the request through the proxy using epoll and returns the response to the client. Timeouts: 2 s connect, 10 s response.
7. **Measure.** Latency, status, proxy and success are recorded for the metrics and the CSV/JSON output.

If an attempt fails, the job is retried through a different proxy, up to 2 times. After 5 consecutive failures a proxy's circuit opens and it is skipped. After a 10 s cooldown, one test request decides whether it returns to service.

### 1.3 Scope

**In scope (core):** scheduler and rate limiter, bounded worker pool with AIMD, proxy manager with all five policies, health monitor, circuit breaker, connection reuse over epoll, metrics, benchmarking, and a web dashboard for demonstration.

**Out of scope:** large machine-learning models, anti-bot evasion, dependence on public proxy lists, a commercial scraping platform, and microsecond or high-frequency-trading performance claims.

**Optional extensions (only after Phase 6 works):** learned proxy selection, kernel and socket tuning, a minimal SOCKS5 proxy node, Grafana. A proxy-health prediction model is deferred.

### 1.4 Targets we are measured against

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

The authoritative specs are the **Requirements and Specification Document v1.0** (use cases UC1 to UC12, functional requirements FR-1 to FR-18) and the **Design Document v1.0**. When the handbook and those documents disagree, record the conflict in `docs/DECISIONS.md` and ask the team before choosing. One known deviation: the SRS says the system has no GUI of its own, but we add a web dashboard to make demonstration easy; this must be written into the next revision of both documents.

## 2. Where to read next

Reading order for every person and agent (handbook rule 1): this file, the latest `HANDOFF_PHASE_N.md`, `PROGRESS.md` (bottom entries first), `DECISIONS.md`, `INTERFACES.md`, `ISSUES.md`. Then build, run the tests, and confirm green before changing anything.
