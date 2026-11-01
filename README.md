# Intelligent Concurrent Proxy Gateway

A C++20/Linux gateway that forwards a high volume of HTTP requests through the best proxy in a managed pool (scheduling, rate limiting, AIMD concurrency, routing policies, health checks and circuit breaking, keep-alive connection reuse, metrics and a demo dashboard). Group 1 project, Computer Networks, IIT Roorkee. Built in six serial phases; see the team handbook.

**Status: Phase 1 (foundation and skeleton) done and verified (tag `phase-1-done`). Next: Phase 2. Start every session with `docs/PROJECT_CONTEXT.md`, then the latest `docs/HANDOFF_PHASE_N.md`.**

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
```

Run by hand:

```bash
testbed/run_local.sh start                         # or: docker compose -f testbed/docker-compose.yml up -d --build
./build/gateway --config config/gateway.local.json # --policy NAME, --workers N, --help
curl -x http://127.0.0.1:8000 http://test.example/products/123
curl http://127.0.0.1:9000/api/stats               # admin API (stub in Phase 1), localhost only
testbed/run_local.sh stop
```

## Layout

| Path | Contents |
| --- | --- |
| `src/common/` | shared structs, interfaces, `Fd` RAII wrapper, clock, config, metrics |
| `src/ingress/` | client listener (port 8000) and HTTP request parser |
| `src/net/` | `Forwarder` implementations (simple blocking forwarder in Phase 1; epoll in Phase 5) |
| `src/gateway/` | glue from `Job` to the forwarder (Phase 1) |
| `src/admin/` | admin HTTP server (stub in Phase 1; live in Phase 6) |
| `src/scheduler/ workers/ routing/ health/ output/` | empty until their phases |
| `tests/` | unit tests; `tests/e2e/` end-to-end acceptance scripts |
| `testbed/` | Docker Compose test bed, local runner, fault-injecting proxies, target server |
| `config/` | `gateway.json` (example), `gateway.local.json`, `gateway.docker.json` |
| `docs/` | tracking folder: context, decisions, progress, handoffs, interfaces, issues |
| `third_party/` | vendored Catch2 and nlohmann/json |
| `dashboard/`, `bench/` | Phase 6 and per-phase benchmarks |

## Ports

8000 gateway client port, 9000 admin and dashboard (localhost only), 9100 control service (Phase 6, test bed only). Test bed: target 18080 (Docker, override with `TARGET_HOST_PORT`) or 8081 (local), proxies 3128 / 3129 / 3130.

## Rules for contributors (and AI agents)

Read `docs/` first, run the build and tests before changing anything, log decisions in `docs/DECISIONS.md` as you make them, log progress in `docs/PROGRESS.md`, never log secrets or response bodies, and finish each phase with a handoff file. Full rules: handbook section 2.
