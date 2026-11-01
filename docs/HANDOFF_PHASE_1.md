# Handoff: Phase 1 Foundation and skeleton

- Owner: Gourav (with a Claude agent)   Dates: 2026-11-01 to 2026-11-01   Branch/tag: `phase-1-foundation` (tag `phase-1-done` NOT yet created, see "Known problems")

**Open this file first. Then read `docs/PROGRESS.md` (bottom), `docs/DECISIONS.md`, `docs/INTERFACES.md`, `docs/ISSUES.md`.**

## What now exists

| Path | What it is |
| --- | --- |
| `CMakeLists.txt` | C++20, `-Wall -Wextra`, option `-DPG_SANITIZE=ON` (ASan + UBSan), library `pg_core`, executable `gateway`, test binary `pg_tests` |
| `third_party/` | Vendored Catch2 v3 and nlohmann/json 3.11.3 (D-001, D-002) |
| `src/common/` | `types.hpp` (Job, ProxyState, DomainQueue, Connection), `interfaces.hpp` (Forwarder, Router, RateLimiter, HealthReporter, ErrorKind, ForwardResult), `metrics.*` (Metrics, MetricsSink), `config.*` (GatewayConfig, loader, validation, CLI), `fd.hpp` (RAII descriptor), `clock.hpp` (Clock, SteadyClock, FakeClock), `http_util.*`, `log.hpp` |
| `src/ingress/` | `listener.*` (port 8000: accept thread, bounded queue, fixed handler pool), `http_parser.*` (absolute-URL HTTP/1.1 parser, 400 on bad input), `http_response.*` (reply building and writing) |
| `src/net/` | `simple_forwarder.*` (Phase 1 forwarder behind `Forwarder`), `http_response_parser.*` (Content-Length, chunked, read-until-close) |
| `src/gateway/` | `GatewayCore`: Job to proxy 0 to forwarder to reply, with metrics and the 502/504 mapping |
| `src/admin/` | Stub admin server on 127.0.0.1:9000: `/health`, `/proxies`, `/api/stats`, `/metrics`, `/ui` (placeholder) |
| `src/main.cpp` | Start-up, signals (SIGPIPE ignored), clean shutdown |
| `src/scheduler/ workers/ routing/ health/ output/` | Empty (`.gitkeep`) |
| `config/` | `gateway.json` (example), `gateway.local.json`, `gateway.docker.json` |
| `testbed/` | `docker-compose.yml`, `target/` (server.py), `proxies/` (faultproxy.py), `gateway/Dockerfile`, `run_local.sh` (no-Docker mode), `verify_testbed.sh` |
| `scripts/` | `smoke.sh` (the demo command), `lib.sh` |
| `tests/` | 71 Catch2 test cases (config, parsers, forwarder, ingress, admin, metrics, fd, clock); `tests/e2e/` end-to-end acceptance scripts |
| `docs/` | The six tracking files plus this handoff |

## How to build, run and test (from a clean checkout)

```bash
git clone <repo> proxy-gateway && cd proxy-gateway && git checkout phase-1-foundation   # or main once merged
cmake -S . -B build && cmake --build build -j       # must print no warnings
./build/pg_tests                                    # 71 test cases pass
cmake -S . -B build-asan -DPG_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-asan -j && ./build-asan/pg_tests
tests/e2e/run_phase1_acceptance.sh                  # add --local to skip Docker
```

Needs: GCC 13 or Clang 18 (what it was built with; GCC 11 UNVERIFIED, I-010), CMake 3.20+, Python 3 (test bed and acceptance scripts, standard library only), curl. Docker is optional.

Run by hand: `testbed/run_local.sh start` (or the Docker command below), then `./build/gateway --config config/gateway.local.json`, then `curl -x http://127.0.0.1:8000 http://test.example/products/123`. Docker test bed: `docker compose -f testbed/docker-compose.yml up -d --build`.

## Demo command and expected result

```bash
scripts/smoke.sh        # uses Docker if a daemon is reachable, else the local test bed
```

Expected last line: `SMOKE PASS: GET http://test.example/products/123 via gateway :8000 -> 200, body identical to the target's (108 bytes)`.

## Acceptance results

Measured on 2026-11-01 from a clean clone, local (no-Docker) test bed, GCC 13.3 on Ubuntu 24.04.

| Done-checklist item | Result | Evidence |
| --- | --- | --- |
| `GET http://test.example/products/123` through port 8000 returns the correct status and body (UC1) | PASS | `scripts/smoke.sh`; e2e check compares the body with the target's own response |
| 1,000 sequential requests return correct bodies | PASS, 1000 of 1000 | e2e check, 1.8 to 1.9 s including the direct comparison requests |
| Malformed request returns HTTP 400 and the gateway keeps running | PASS | 9 malformed cases (garbage, relative URL, bad version, bad method, https URL, bad header, conflicting Content-Length, 40 KB header, truncated request) all returned 400; gateway then still served correct bodies; 20 client connections reset mid-request did not hurt it |
| Invalid config rejected with a clear message and non-zero exit code (UC9) | PASS | 7 invalid files, a missing file and an invalid `--policy` all exit with code 2 and a `gateway: error: config: ...` message; list in the acceptance script output |
| `docker compose up` starts the full test bed with one command | **UNVERIFIED** | No Docker daemon was available. Compose file is valid YAML; the same servers work in local mode (I-002) |
| Delay on Proxy 2 and failures on Proxy 3 verified with direct requests | PASS (local mode) | `testbed/verify_testbed.sh`: proxy2 minus proxy1 average latency 100.6 to 100.9 ms (50 requests each); proxy3 failed 14.5, 18.0 and 21.0 percent of 200 requests in three runs; proxy1 0 percent |
| Sanitizer build passes the unit tests with no errors | PASS | 71 test cases, 1,250 assertions with `-fsanitize=address,undefined`; the acceptance script was also run against the sanitizer-built gateway with no sanitizer output |
| `docs/` files exist, decisions logged, handoff written | PASS | This folder |

Extra results: no file-descriptor leak across 1,000 requests (open fds 5 before and 5 after); gateway log contained no response bodies or request headers; the unit test for the lingering close fails without the fix and passes with it (D-017).

Not measured in Phase 1 (do not quote as achieved): gateway overhead, throughput, concurrency and memory targets. They belong to Phase 5.

## Interfaces added or changed

Everything in `docs/INTERFACES.md` is new in this phase: structs, module interfaces, config keys, gateway status codes, admin API shapes (`/health`, `/proxies`, `/api/stats`, `/metrics`) and test-bed interfaces.

## Decisions made

D-001 to D-017 (see `docs/DECISIONS.md`). The ones later phases are most likely to touch: D-005 (ingress threading), D-006 (simple forwarder), D-008 (status codes), D-010 (success definition), D-015 (admin API), D-017 (lingering close).

## Known problems and shortcuts

See `docs/ISSUES.md` (I-001 to I-012). The important ones:

- **I-002 / I-010: the Docker test bed and the gateway Dockerfile have never run.** Phase 1's `phase-1-done` tag and the merge to `main` are waiting for this (D-003).
- I-003: ingress is synchronous, so in-flight requests are capped at 16 until Phase 2.
- I-005: routing policy, workers, rate limits, circuit and weights are accepted and validated but not used yet.
- I-007: the test-bed target and proxies are Python, which may cap throughput results.

## First things the next owner should do

1. If you have Docker: run `scripts/smoke.sh` and `testbed/verify_testbed.sh` with the Docker test bed, fix anything that breaks, record the result in `docs/PROGRESS.md` and close I-002. Then merge `phase-1-foundation` into `main` and tag `phase-1-done` (handbook 2.4 step 5).
2. Read the handbook's Phase 2 section, then build and run the tests to confirm everything is green.
3. Phase 2 starting points: replace the synchronous `Listener::Handler` (`src/main.cpp` builds it; `GatewayCore::handle` is the current handler) with "enqueue the Job into its domain queue". The reply must be written later by a worker with `write_response(job.client_fd, ...)`, so the ingress must keep the client socket open until then (today the handler thread owns it and closes it on return; change that ownership deliberately and log a decision). Keep the lingering close for 400 replies (D-017).
4. Add Phase 2 counters by appending to `enum class Counter` in `src/common/metrics.hpp` (before `kCount`) and publish them through the existing admin field names in `docs/INTERFACES.md` (`queue_length`, `worker_count`, `aimd_limit`).
5. For the per-domain rate-limit test the target already answers to `test.example`, `site-a.example` and `site-b.example`, and `GET /stats` on the target returns request counts per host and per second (prune it, see I-011). The example config already has `rate_limits."site-a.example"`.
