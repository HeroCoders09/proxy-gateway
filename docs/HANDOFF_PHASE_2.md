# Handoff: Phase 2 Scheduling and Concurrency

Open this file first, then `docs/PROGRESS.md`, `docs/DECISIONS.md`, `docs/INTERFACES.md`, and `docs/ISSUES.md`.

## Delivered

- `DomainScheduler`: bounded FIFO queues and independent per-domain token buckets. A full queue replies 503.
- Ingress transfers the RAII client socket to the scheduler; the worker pool writes the eventual response.
- Fixed pool of exactly `workers.max` threads, gated by AIMD concurrency C.
- AIMD uses one-second windows and a 10% error threshold (D-019).
- Live `/api/stats` and `/metrics` values for queues, worker count, active jobs, AIMD limit, and `queue_rejected`.
- `bench/phase2_verify.py` verifies rate isolation and fault/recovery.

## Verification

`cmake -S . -B build && cmake --build build -j && ./build/pg_tests && scripts/smoke.sh --local`

Normal and sanitizer runs: 74 cases / 1,267 assertions. `python3 bench/phase2_verify.py` recorded: site A max 50/s, site B 100 requests, AIMD 8 -> 4 -> 6.

## For Phase 3

`GatewayCore` still chooses proxy 0. Replace that choice behind `Router`; retain scheduler and worker ownership boundaries. Start by reading this handoff.
