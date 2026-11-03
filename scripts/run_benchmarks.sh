#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="./build"
SUMMARY_CSV="benchmark_summary.csv"
TOTAL_REQ=200

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"
cmake .. >/dev/null
make -j"$(nproc)" >/dev/null
cd ..

echo "policy,p50_ms,p95_ms,p99_ms,rps,success_rate" > "${SUMMARY_CSV}"

cleanup() {
    pkill -f "./build/gateway" 2>/dev/null || true
    pkill -f "./build/proxy_node" 2>/dev/null || true
    pkill -f "./build/target_server" 2>/dev/null || true
}
trap cleanup EXIT

cleanup
./build/target_server --port 9000 &
./build/proxy_node --port 8101 --delay-ms 5 --fail-rate 0.0 &
./build/proxy_node --port 8102 --delay-ms 80 --fail-rate 0.0 &
./build/proxy_node --port 8103 --delay-ms 20 --fail-rate 0.25 &
sleep 1

for POLICY in random round_robin least_connections latency weighted; do
    echo "[Benchmark] Running policy: ${POLICY}..."
    ./build/gateway --config config/gateway.json --policy "${POLICY}" --workers 24 >/dev/null 2>&1 &
    GW_PID=$!
    sleep 1

    # Issue concurrent requests via gateway proxy port 8000
    seq 1 "${TOTAL_REQ}" | xargs -P16 -I{} \
        curl -s -o /dev/null -x http://127.0.0.1:8000 --noproxy "" http://127.0.0.1:9000/products/{} || true

    METRICS=$(curl -s http://127.0.0.1:8000/metrics)
    P50=$(echo "${METRICS}" | awk '/quantile="0.50"/ {print $2}')
    P95=$(echo "${METRICS}" | awk '/quantile="0.95"/ {print $2}')
    P99=$(echo "${METRICS}" | awk '/quantile="0.99"/ {print $2}')
    RPS=$(echo "${METRICS}" | awk '/^requests_per_second/ {print $2}')
    SUCC=$(echo "${METRICS}" | awk '/^requests_successful/ {print $2}')
    TOT=$(echo "${METRICS}" | awk '/^requests_total/ {print $2}')
    SR=$(awk -v s="${SUCC:-0}" -v t="${TOT:-1}" 'BEGIN { printf "%.4f", (t>0 ? s/t : 0) }')

    echo "${POLICY},${P50},${P95},${P99},${RPS},${SR}" >> "${SUMMARY_CSV}"
    kill "${GW_PID}" 2>/dev/null || true
    wait "${GW_PID}" 2>/dev/null || true
done

echo "[Benchmark] Complete! Summary table saved to ${SUMMARY_CSV}:"
if command -v column >/dev/null 2>&1; then column -s, -t "${SUMMARY_CSV}"; else cat "${SUMMARY_CSV}"; fi
