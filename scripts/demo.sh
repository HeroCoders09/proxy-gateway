#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
RUN="$ROOT/testbed/.run"
mkdir -p "$RUN"
case "${1:-}" in
  start)
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
    ;;
  status)
    "$ROOT/testbed/run_local.sh" status
    if [ -f "$RUN/gateway.pid" ] && kill -0 "$(cat "$RUN/gateway.pid")" 2>/dev/null; then echo "gateway: running"; else echo "gateway: stopped"; fi
    ;;
  *) echo "usage: scripts/demo.sh start|stop|status" >&2; exit 2 ;;
esac
