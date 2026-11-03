#!/usr/bin/env bash
# Run the test bed as plain processes (no Docker). Same servers, same behaviour:
#   target  127.0.0.1:8081   (Docker: 8080)   answers to test.example, site-a.example, site-b.example
#   proxy1  127.0.0.1:3128   normal
#   proxy2  127.0.0.1:3129   +100 ms delay
#   proxy3  127.0.0.1:3130   ~20 percent failures
#   proxy4  127.0.0.1:3131   +250 ms delay
#   proxy5  127.0.0.1:3132   ~40 percent failures
#   fault-control ports 3228 through 3232
# Usage: testbed/run_local.sh start|stop|status
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN="$HERE/.run"
TARGET_PORT="${TARGET_PORT:-8081}"
OVERRIDES="test.example=127.0.0.1:$TARGET_PORT,site-a.example=127.0.0.1:$TARGET_PORT,site-b.example=127.0.0.1:$TARGET_PORT,northstar.pricelens.test=127.0.0.1:$TARGET_PORT,harbor.pricelens.test=127.0.0.1:$TARGET_PORT,copperlane.pricelens.test=127.0.0.1:$TARGET_PORT"
mkdir -p "$RUN"

start_one() {  # name, command...
  local name="$1"; shift
  if [ -f "$RUN/$name.pid" ] && kill -0 "$(cat "$RUN/$name.pid")" 2>/dev/null; then return; fi
  nohup "$@" >"$RUN/$name.log" 2>&1 &
  echo $! >"$RUN/$name.pid"
}

wait_port() {  # port
  for _ in $(seq 1 50); do
    (exec 3<>/dev/tcp/127.0.0.1/"$1") 2>/dev/null && return 0
    sleep 0.1
  done
  return 1
}

case "${1:-}" in
  start)
    start_one target env PORT="$TARGET_PORT" BIND=127.0.0.1 python3 "$HERE/target/server.py"
    start_one proxy1 env PROXY_ID=proxy1 PORT=3128 CONTROL_PORT=3228 BIND=127.0.0.1 HOST_OVERRIDES="$OVERRIDES" python3 "$HERE/proxies/faultproxy.py"
    start_one proxy2 env PROXY_ID=proxy2 PORT=3129 CONTROL_PORT=3229 BIND=127.0.0.1 HOST_OVERRIDES="$OVERRIDES" DELAY_MS=100 python3 "$HERE/proxies/faultproxy.py"
    start_one proxy3 env PROXY_ID=proxy3 PORT=3130 CONTROL_PORT=3230 BIND=127.0.0.1 HOST_OVERRIDES="$OVERRIDES" FAIL_PCT=20 python3 "$HERE/proxies/faultproxy.py"
    start_one proxy4 env PROXY_ID=proxy4 PORT=3131 CONTROL_PORT=3231 BIND=127.0.0.1 HOST_OVERRIDES="$OVERRIDES" DELAY_MS=250 python3 "$HERE/proxies/faultproxy.py"
    start_one proxy5 env PROXY_ID=proxy5 PORT=3132 CONTROL_PORT=3232 BIND=127.0.0.1 HOST_OVERRIDES="$OVERRIDES" FAIL_PCT=40 python3 "$HERE/proxies/faultproxy.py"
    start_one control env CONTROL_BIND=127.0.0.1 CONTROL_PORT=9100 python3 "$HERE/control-service/control_service.py"
    for p in "$TARGET_PORT" 3128 3129 3130 3131 3132 9100; do
      wait_port "$p" || { echo "test bed: port $p did not come up (see $RUN/*.log)" >&2; exit 1; }
    done
    echo "test bed (local mode) is up: target :$TARGET_PORT, proxies :3128 :3129 :3130 :3131 :3132, control :9100"
    ;;
  stop)
    for f in "$RUN"/*.pid; do
      [ -f "$f" ] || continue
      pid="$(cat "$f")"
      kill -CONT "$pid" 2>/dev/null || true
      kill "$pid" 2>/dev/null || true
      rm -f "$f"
    done
    echo "test bed (local mode) stopped"
    ;;
  status)
    for f in "$RUN"/*.pid; do
      [ -f "$f" ] || { echo "not running"; exit 0; }
      n="$(basename "$f" .pid)"
      if kill -0 "$(cat "$f")" 2>/dev/null; then echo "$n: running"; else echo "$n: stopped"; fi
    done
    ;;
  *) echo "usage: $0 start|stop|status" >&2; exit 2 ;;
esac
