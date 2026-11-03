#!/usr/bin/env bash
# Build and run the gateway with saved external upstream proxies, then open the dashboard.
# Usage: ./scripts/run_real_proxy_demo.sh
# Optional: GATEWAY_CONFIG=/path/to/gateway.json BUILD_DIR=/path/to/build ./scripts/run_real_proxy_demo.sh
set -Eeuo pipefail
umask 077

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
CONFIG_HOME="${XDG_CONFIG_HOME:-${HOME:-}/.config}"
if [[ -z "$CONFIG_HOME" ]]; then
    echo "HOME or XDG_CONFIG_HOME must be set to locate the dashboard configuration." >&2
    exit 1
fi
CONFIG_PATH="${GATEWAY_CONFIG:-$CONFIG_HOME/proxy-gateway/gateway.json}"
STATE_HOME="${XDG_STATE_HOME:-${HOME:-}/.local/state}"
if [[ -z "$STATE_HOME" ]]; then
    echo "HOME or XDG_STATE_HOME must be set to locate the dashboard state directory." >&2
    exit 1
fi
STATE_DIR="$STATE_HOME/proxy-gateway"
PID_FILE="$STATE_DIR/gateway.pid"
LOG_FILE="$STATE_DIR/gateway.log"
GATEWAY_PID=""
UI_PID=""

cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$UI_PID" ]] && kill -0 "$UI_PID" 2>/dev/null; then
        kill -TERM "$UI_PID" 2>/dev/null || true
        wait "$UI_PID" 2>/dev/null || true
    fi
    local recorded_pid=""
    if [[ -f "$PID_FILE" ]]; then recorded_pid="$(cat "$PID_FILE" 2>/dev/null || true)"; fi
    local managed_pids=()
    if [[ -n "$GATEWAY_PID" ]]; then managed_pids+=("$GATEWAY_PID"); fi
    if [[ "$recorded_pid" =~ ^[0-9]+$ && "$recorded_pid" != "$GATEWAY_PID" && -r "/proc/$recorded_pid/cmdline" ]]; then
        local recorded_cmd
        recorded_cmd="$(tr '\0' ' ' < "/proc/$recorded_pid/cmdline" 2>/dev/null || true)"
        if [[ "$recorded_cmd" == *"$BUILD_DIR/gateway"* ]]; then managed_pids+=("$recorded_pid"); fi
    fi
    for pid in "${managed_pids[@]}"; do
        if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
    for pid in "${managed_pids[@]}"; do
        [[ "$pid" =~ ^[0-9]+$ ]] || continue
        for _ in {1..60}; do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    if [[ -f "$PID_FILE" ]] && [[ "$(cat "$PID_FILE" 2>/dev/null || true)" == "$GATEWAY_PID" || "$(cat "$PID_FILE" 2>/dev/null || true)" == "$recorded_pid" ]]; then
        rm -f -- "$PID_FILE"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ ! -f "$CONFIG_PATH" ]]; then
    echo "No saved dashboard config found at: $CONFIG_PATH" >&2
    echo "Start the dashboard once, add your real proxy endpoints, and save the config." >&2
    echo "You can also choose a file with GATEWAY_CONFIG=/path/to/gateway.json." >&2
    exit 1
fi

python3 - "$CONFIG_PATH" <<'PY'
import ipaddress, json, sys
try:
    with open(sys.argv[1], encoding="utf-8") as stream:
        config = json.load(stream)
except Exception as exc:
    print(f"Cannot read proxy configuration: {exc}", file=sys.stderr)
    sys.exit(1)
proxies = config.get("proxies") if isinstance(config, dict) else None
if not isinstance(proxies, list) or not proxies:
    print("No upstream proxies are configured. Add and save at least one proxy in the dashboard first.", file=sys.stderr)
    sys.exit(1)
external = 0
for proxy in proxies:
    host = proxy.get("host", "") if isinstance(proxy, dict) else ""
    try:
        local = host.lower() == "localhost" or ipaddress.ip_address(host.strip("[]")).is_loopback
    except (ValueError, AttributeError):
        local = False
    if not local:
        external += 1
if not external:
    print("This config has only local demo proxies. Use ./scripts/run_local_demo.sh for that setup.", file=sys.stderr)
    sys.exit(1)
print(f"Found {external} external upstream proxy endpoint(s) in {sys.argv[1]}.")
print("The remote proxy services must already be running and reachable.")
PY

cd "$ROOT"
echo "Building the gateway..."
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target gateway -j"$(nproc)"

echo "Validating the saved gateway configuration..."
"$BUILD_DIR/gateway" --config "$CONFIG_PATH" --check-config

LISTEN_PORT="$(python3 - "$CONFIG_PATH" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as stream:
    print(json.load(stream).get("listen_port", 8000))
PY
)"
if python3 - "$LISTEN_PORT" <<'PY'
import socket, sys
s = socket.socket(); s.settimeout(.25)
busy = s.connect_ex(("127.0.0.1", int(sys.argv[1]))) == 0
s.close()
sys.exit(0 if busy else 1)
PY
then
    echo "Port $LISTEN_PORT already has a listener. Stop it before starting this gateway." >&2
    exit 1
fi

mkdir -p "$STATE_DIR"
if [[ -f "$PID_FILE" ]]; then
    OLD_PID="$(cat "$PID_FILE" 2>/dev/null || true)"
    if [[ "$OLD_PID" =~ ^[0-9]+$ ]] && kill -0 "$OLD_PID" 2>/dev/null; then
        echo "A gateway process is already recorded at $PID_FILE (PID $OLD_PID). Stop it first." >&2
        exit 1
    fi
    rm -f -- "$PID_FILE"
fi

echo "Starting the gateway with the configured external proxies..."
"$BUILD_DIR/gateway" --config "$CONFIG_PATH" >>"$LOG_FILE" 2>&1 &
GATEWAY_PID=$!
printf '%s\n' "$GATEWAY_PID" > "$PID_FILE"
sleep 0.5
if ! kill -0 "$GATEWAY_PID" 2>/dev/null; then
    echo "Gateway failed to start. Recent log output:" >&2
    tail -n 30 "$LOG_FILE" >&2 || true
    exit 1
fi

echo "Opening the dashboard with the running gateway."
echo "Press Ctrl+C here to close the dashboard and stop the gateway."
python3 "$ROOT/app/gateway_ui.py" \
    --bin "$BUILD_DIR/gateway" \
    --config "$CONFIG_PATH" &
UI_PID=$!
wait "$UI_PID"
