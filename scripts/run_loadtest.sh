#!/usr/bin/env bash
# Heavy-load test: starts the test bed, then drives the gateway with `loadgen`.
#   ./scripts/run_loadtest.sh [CONNS=256] [DURATION=15] [METHOD=GET] [BODY_BYTES=0]
set -euo pipefail
CONNS="${1:-256}"; DURATION="${2:-15}"; METHOD="${3:-GET}"; BODY="${4:-0}"
BUILD="${BUILD_DIR:-./build}"
cleanup() { kill ${PIDS:-} 2>/dev/null || true; wait 2>/dev/null || true; }
trap cleanup EXIT
"${BUILD}/target_server" --port 9000 >/dev/null & PIDS="$!"
"${BUILD}/proxy_node" --port 8101 --delay-ms 0 >/dev/null & PIDS="$PIDS $!"
"${BUILD}/proxy_node" --port 8102 --delay-ms 0 >/dev/null & PIDS="$PIDS $!"
"${BUILD}/proxy_node" --port 8103 --delay-ms 0 >/dev/null & PIDS="$PIDS $!"
sleep 0.5
"${BUILD}/gateway" --config config/loadtest.json >/dev/null & PIDS="$PIDS $!"
sleep 1
"${BUILD}/loadgen" --proxy-port 8000 --url http://127.0.0.1:9000/products/1 --conns "${CONNS}" \
    --duration "${DURATION}" --method "${METHOD}" --body-bytes "${BODY}"
curl --noproxy '' -s http://127.0.0.1:8000/metrics | grep -E '^(requests_|timeouts|queue_timeouts|client_connections|worker_count)'
