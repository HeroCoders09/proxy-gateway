# Intelligent Concurrent Proxy Gateway

A C++20/Linux gateway that forwards a high volume of HTTP requests through the best proxy in a managed pool (scheduling, rate limiting, AIMD concurrency, routing policies, health checks and circuit breaking, keep-alive connection reuse, metrics and a demo dashboard). Group 1 project, Computer Networks, IIT Roorkee. Built in six serial phases; see the team handbook.

**Status: Phase 6 software acceptance is complete on `codex/real-world-gateway`; Phase 5 performance acceptance and final project sign-off remain open.** The productization branch adds optional client authentication, exact egress host/port policies, per-upstream Basic auth from environment variables, and an optional TLS front end for Docker. Local and Docker TLS flows and Phase 6 dashboard/benchmark checks have been verified. Destination DNS/IP enforcement and TLS to upstream proxy providers remain open before internet-facing use. See [`docs/HANDOFF_PRODUCTIZATION.md`](docs/HANDOFF_PRODUCTIZATION.md) for productization limits, and [`docs/HANDOFF_PHASE_6.md`](docs/HANDOFF_PHASE_6.md) for acceptance evidence.

## Quick start

```bash
# build and run the unit tests (Catch2)
cmake -S . -B build && cmake --build build -j && ./build/pg_tests

# sanitizer build (AddressSanitizer + UndefinedBehaviorSanitizer): run before every handoff
cmake -S . -B build-asan -DPG_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-asan -j && ./build-asan/pg_tests

# demo command: test bed + gateway + one request through port 8000 (uses Docker if available)
scripts/smoke.sh            # add --local to force the no-Docker test bed

# full Phase 1 acceptance run (invalid configs, 1,000 requests, malformed input, fd leaks, fault behaviour)
tests/e2e/run_phase1_acceptance.sh

# Phase 6 local dashboard demo (build first, then start the local test bed and gateway)
./scripts/demo.sh start
# open http://127.0.0.1:9000/ui
# stop with ./scripts/demo.sh stop
```

The PriceLens dashboard monitors synthetic offers across three fictional retailer hosts through the local fault-injecting test proxies. It uses generated Basic client credentials and an exact host/port egress allowlist. It can change bounded proxy delay/failure settings, apply named scenarios, switch routing/rate settings, and start the offer monitor or benchmark. See [`docs/DEMO.md`](docs/DEMO.md) for the walkthrough and [`docs/HANDOFF_PHASE_6.md`](docs/HANDOFF_PHASE_6.md) for validation status.

Run by hand:

```bash
testbed/run_local.sh start                         # or: docker compose -f testbed/docker-compose.yml up -d --build
./build/gateway --config config/gateway.local.json # set "forwarder": "epoll" to use Phase 5 reactor
curl -x http://127.0.0.1:8000 http://test.example/products/123
curl http://127.0.0.1:9000/api/stats               # live admin API, localhost only
testbed/run_local.sh stop
```

## Layout

| Path | Contents |
| --- | --- |
| `src/common/` | shared structs, interfaces, `Fd` RAII wrapper, clock, config, metrics |
| `src/ingress/` | client listener (port 8000) and HTTP request parser |
| `src/net/` | `Forwarder` implementations (simple blocking forwarder in Phase 1; epoll in Phase 5) |
| `src/gateway/` | glue from `Job` to the forwarder (Phase 1) |
| `src/admin/` | live admin API and dashboard hosting (Phase 6) |
| `src/output/` | bounded asynchronous CSV/JSONL request-result output (Phase 6) |
| `src/scheduler/ workers/ routing/ health/` | domain scheduling, AIMD workers, routing policies and health/circuit monitoring |
| `tests/` | unit tests; `tests/e2e/` end-to-end acceptance scripts |
| `testbed/` | Docker Compose test bed, local runner, fault-injecting proxies, target server |
| `config/` | `gateway.json` (example), `gateway.local.json`, `gateway.docker.json` |
| `docs/` | tracking folder: context, decisions, progress, handoffs, interfaces, issues |
| `third_party/` | vendored Catch2 and nlohmann/json |
| `dashboard/`, `bench/` | live dashboard, load generator and benchmark suites |

## Ports

8000 gateway client port, 9000 admin and dashboard (localhost only), 9100 control service (Phase 6, test bed only). Test bed: target 18080 (Docker, override with `TARGET_HOST_PORT`) or 8081 (local), five proxies on 3128–3132 and their fault controls on 3228–3232.

## Rules for contributors (and AI agents)

Read `docs/` first, run the build and tests before changing anything, log decisions in `docs/DECISIONS.md` as you make them, log progress in `docs/PROGRESS.md`, never log secrets or response bodies, and finish each phase with a handoff file. Full rules: handbook section 2.
