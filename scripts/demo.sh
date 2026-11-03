#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
RUN="$ROOT/testbed/.run"
mkdir -p "$RUN"
case "${1:-}" in
  start)
    CLIENT_SECRET_FILE="$RUN/client-password"
    if [ -z "${PROXY_GATEWAY_CLIENT_PASSWORD:-}" ]; then
      if [ ! -s "$CLIENT_SECRET_FILE" ]; then
        (umask 077; python3 -c 'import secrets; print(secrets.token_urlsafe(32))' >"$CLIENT_SECRET_FILE")
      fi
      chmod 600 "$CLIENT_SECRET_FILE"
      IFS= read -r PROXY_GATEWAY_CLIENT_PASSWORD <"$CLIENT_SECRET_FILE"
      echo "Using local client credentials (username: pricelens); password stored at testbed/.run/client-password"
    else
      echo "Using the supplied PROXY_GATEWAY_CLIENT_PASSWORD for local client authentication"
    fi
    if [ "${#PROXY_GATEWAY_CLIENT_PASSWORD}" -lt 16 ]; then
      echo "PROXY_GATEWAY_CLIENT_PASSWORD must contain at least 16 characters" >&2
      exit 2
    fi
    export PROXY_GATEWAY_CLIENT_PASSWORD
    "$ROOT/testbed/run_local.sh" start
    if [ ! -x "$ROOT/build/gateway" ]; then
      cmake -S "$ROOT" -B "$ROOT/build"
      cmake --build "$ROOT/build" -j
    fi
    if [ -f "$RUN/gateway.pid" ] && kill -0 "$(cat "$RUN/gateway.pid")" 2>/dev/null; then
      echo "gateway already running"
    else
      nohup "$ROOT/build/gateway" --config "$ROOT/config/gateway.demo.json" >"$RUN/gateway.log" 2>&1 &
      echo $! >"$RUN/gateway.pid"
    fi
    for _ in $(seq 1 80); do
      if (exec 3<>/dev/tcp/127.0.0.1/9000) 2>/dev/null; then
        echo "Demo ready: open http://127.0.0.1:9000/ui"
        exit 0
      fi
      sleep 0.1
    done
    echo "gateway admin did not become ready; see $RUN/gateway.log" >&2
    exit 1
    ;;
  stop)
    if [ -f "$RUN/gateway.pid" ]; then
      kill "$(cat "$RUN/gateway.pid")" 2>/dev/null || true
      rm -f "$RUN/gateway.pid"
    fi
    "$ROOT/testbed/run_local.sh" stop
    rm -f "$RUN/client-password"
    ;;
  status)
    "$ROOT/testbed/run_local.sh" status
    if [ -f "$RUN/gateway.pid" ] && kill -0 "$(cat "$RUN/gateway.pid")" 2>/dev/null; then echo "gateway: running"; else echo "gateway: stopped"; fi
    ;;
  *) echo "usage: scripts/demo.sh start|stop|status" >&2; exit 2 ;;
esac
