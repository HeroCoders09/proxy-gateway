# Shared helpers for scripts/smoke.sh and tests/e2e/phase1_acceptance.sh. Source this file.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GATEWAY_BIN="${GATEWAY_BIN:-$ROOT/build/gateway}"
GW_CONFIG="${GW_CONFIG:-$ROOT/config/gateway.local.json}"
GW_PORT=8000
ADMIN_PORT=9000
GW_PID=""
TESTBED_MODE=""       # docker | local
TARGET_DIRECT_PORT="" # where to reach the target directly from this machine

have_docker() { command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; }

build_gateway() {
  if [ ! -x "$GATEWAY_BIN" ]; then
    echo "building gateway ..."
    cmake -S "$ROOT" -B "$ROOT/build" >/dev/null && cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null || return 1
  fi
}

start_testbed() {  # arg: --local to force local mode
  if [ "${1:-}" != "--local" ] && have_docker; then
    TESTBED_MODE=docker; export TARGET_HOST_PORT="${TARGET_HOST_PORT:-18080}"; TARGET_DIRECT_PORT="$TARGET_HOST_PORT"
    docker compose -f "$ROOT/testbed/docker-compose.yml" up -d --build --wait >/dev/null || {
      echo "docker compose failed. Common cause: a host port is taken (target $TARGET_HOST_PORT, proxies 3128-3132, control 3228-3232)."
      echo "Check with: ss -ltnp | grep -E ':(${TARGET_HOST_PORT}|3128|3129|3130|3131|3132)\\b'   Override the target port with TARGET_HOST_PORT=19090"
      return 1; }
  else
    TESTBED_MODE=local; TARGET_DIRECT_PORT=8081
    "$ROOT/testbed/run_local.sh" start || return 1
  fi
  echo "test bed mode: $TESTBED_MODE"
}

stop_testbed() {
  if [ "$TESTBED_MODE" = docker ]; then docker compose -f "$ROOT/testbed/docker-compose.yml" down >/dev/null 2>&1
  elif [ "$TESTBED_MODE" = local ]; then "$ROOT/testbed/run_local.sh" stop >/dev/null
  fi
}

start_gateway() {  # extra args are passed to the gateway
  "$GATEWAY_BIN" --config "$GW_CONFIG" "$@" >"${TMPDIR:-/tmp}/gateway_smoke.log" 2>&1 &
  GW_PID=$!
  for _ in $(seq 1 50); do
    (exec 3<>/dev/tcp/127.0.0.1/$GW_PORT) 2>/dev/null && return 0
    kill -0 "$GW_PID" 2>/dev/null || { echo "gateway exited early:"; cat "${TMPDIR:-/tmp}/gateway_smoke.log"; return 1; }
    sleep 0.1
  done
  echo "gateway did not open port $GW_PORT"; return 1
}

stop_gateway() { [ -n "$GW_PID" ] && kill "$GW_PID" 2>/dev/null && wait "$GW_PID" 2>/dev/null; GW_PID=""; }

# Expected body, fetched straight from the target (no gateway, no proxy).
direct_body() {  # path
  curl -s -H "Host: test.example" "http://127.0.0.1:$TARGET_DIRECT_PORT$1"
}
