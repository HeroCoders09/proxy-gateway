#!/usr/bin/env bash
# Build and run the local gateway demo with its local target and three test proxies.
# Usage: ./scripts/run_local_demo.sh
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
TMP_DIR=""
LOCAL_PIDS=()
UI_PID=""

cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$UI_PID" ]] && kill -0 "$UI_PID" 2>/dev/null; then
        kill -TERM "$UI_PID" 2>/dev/null || true
        wait "$UI_PID" 2>/dev/null || true
    fi
    for pid in "${LOCAL_PIDS[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in "${LOCAL_PIDS[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    if [[ -n "$TMP_DIR" ]]; then
        rm -rf -- "$TMP_DIR"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

port_is_free() {
    python3 - "$1" <<'PY'
import socket, sys
sock = socket.socket()
sock.settimeout(0.25)
try:
    # A successful bind can be prevented by stale TCP TIME_WAIT sockets even
    # when no service is listening. Only treat an accepting listener as busy.
    if sock.connect_ex(("127.0.0.1", int(sys.argv[1]))) == 0:
        sys.exit(1)
finally:
    sock.close()
PY
}

wait_for_port() {
    local port="$1" pid="$2"
    for _ in {1..50}; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "A local demo service exited during startup (port $port)." >&2
            return 1
        fi
        if python3 - "$port" <<'PY' >/dev/null 2>&1
import socket, sys
sock = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.2)
sock.close()
PY
        then
            return 0
        fi
        sleep 0.1
    done
    echo "Timed out waiting for local demo service on port $port." >&2
    return 1
}

cd "$ROOT"

for port in 8000 8101 8102 8103 9000; do
    if ! port_is_free "$port"; then
        echo "Port $port is already in use. Stop the service using it, then run this script again." >&2
        exit 1
    fi
done

echo "Building gateway and local demo programs..."
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/proxy-gateway-demo.XXXXXX")"
cp "$ROOT/config/gateway.json" "$TMP_DIR/gateway.json"

echo "Starting local target and test proxies..."
"$BUILD_DIR/target_server" --port 9000 >"$TMP_DIR/target.log" 2>&1 &
target_pid=$!
LOCAL_PIDS+=("$target_pid")

"$BUILD_DIR/proxy_node" --port 8101 --delay-ms 45 --fail-rate 0.10 >"$TMP_DIR/proxy-8101.log" 2>&1 &
proxy1_pid=$!
LOCAL_PIDS+=("$proxy1_pid")

"$BUILD_DIR/proxy_node" --port 8102 --delay-ms 100 --fail-rate 0.05 >"$TMP_DIR/proxy-8102.log" 2>&1 &
proxy2_pid=$!
LOCAL_PIDS+=("$proxy2_pid")

"$BUILD_DIR/proxy_node" --port 8103 --delay-ms 30 --fail-rate 0.30 >"$TMP_DIR/proxy-8103.log" 2>&1 &
proxy3_pid=$!
LOCAL_PIDS+=("$proxy3_pid")

wait_for_port 9000 "$target_pid"
wait_for_port 8101 "$proxy1_pid"
wait_for_port 8102 "$proxy2_pid"
wait_for_port 8103 "$proxy3_pid"

echo "Launching the dashboard with local proxy nodes."
echo "The dashboard will use a temporary copy of config/gateway.json."
echo "Press Ctrl+C here to stop the dashboard and the local demo services."
python3 "$ROOT/app/gateway_ui.py" \
    --bin "$BUILD_DIR/gateway" \
    --config "$TMP_DIR/gateway.json" &
UI_PID=$!
wait "$UI_PID"
