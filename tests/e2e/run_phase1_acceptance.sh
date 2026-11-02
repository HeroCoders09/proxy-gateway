#!/usr/bin/env bash
# Runs the Phase 1 done checklist items that can be checked automatically:
#   UC1, 1,000 sequential requests, malformed -> 400, invalid config rejected (UC9), fd-leak check,
#   admin live API shapes, test-bed fault behaviour (proxy2 +100 ms, proxy3 ~20 % failures).
# Usage: tests/e2e/run_phase1_acceptance.sh [--local]
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../../scripts/lib.sh"
trap 'stop_gateway; stop_testbed; rm -rf "$BADDIR"' EXIT
BADDIR="$(mktemp -d)"
overall=0

build_gateway || { echo "FAIL build"; exit 1; }

echo "== UC9: invalid configuration is rejected with a clear message and a non-zero exit code"
write_bad() { printf '%s' "$2" >"$BADDIR/$1.json"; }
write_bad not_json       '{ this is not json'
write_bad no_proxies     '{"proxies": []}'
write_bad bad_port       '{"listen":{"port":70000},"proxies":[{"host":"a","port":3128}]}'
write_bad bad_policy     '{"policy":"magic","proxies":[{"host":"a","port":3128}]}'
write_bad bad_beta       '{"workers":{"beta":1.5},"proxies":[{"host":"a","port":3128}]}'
write_bad min_gt_max     '{"workers":{"min":10,"max":2},"proxies":[{"host":"a","port":3128}]}'
write_bad neg_timeout    '{"timeouts":{"connect_ms":-5},"proxies":[{"host":"a","port":3128}]}'
for f in "$BADDIR"/*.json; do
  name="$(basename "$f" .json)"
  out="$("$GATEWAY_BIN" --config "$f" 2>&1 >/dev/null)"; rc=$?
  if [ $rc -ne 0 ] && [ -n "$out" ]; then echo "PASS $name rejected, exit code $rc: $out"; else echo "FAIL $name was not rejected (exit $rc)"; overall=1; fi
done
"$GATEWAY_BIN" --config /nonexistent.json >/dev/null 2>&1; rc=$?
[ $rc -ne 0 ] && echo "PASS missing config file rejected, exit code $rc" || { echo "FAIL missing config accepted"; overall=1; }
"$GATEWAY_BIN" --config "$GW_CONFIG" --policy nonsense >/dev/null 2>&1; rc=$?
[ $rc -ne 0 ] && echo "PASS invalid --policy rejected, exit code $rc" || { echo "FAIL invalid --policy accepted"; overall=1; }
"$GATEWAY_BIN" --help | grep -q -- "--config" && echo "PASS --help works" || { echo "FAIL --help"; overall=1; }

echo "== test bed"
start_testbed "${1:-}" || { echo "FAIL test bed did not start"; exit 1; }
echo "== test-bed fault behaviour (direct requests)"
"$ROOT/testbed/verify_testbed.sh" || overall=1

echo "== gateway end to end"
start_gateway || { echo "FAIL gateway did not start"; exit 1; }
GW_PID="$GW_PID" TARGET_PORT="$TARGET_DIRECT_PORT" python3 "$ROOT/tests/e2e/phase1_checks.py" || overall=1

echo "== gateway log contains no response bodies or request headers"
if grep -qiE "<html|price:|Host: " "${TMPDIR:-/tmp}/gateway_smoke.log"; then echo "FAIL log leaks content"; overall=1; else echo "PASS gateway log holds metadata only"; fi

[ $overall -eq 0 ] && echo "ALL PHASE 1 AUTOMATED CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $overall
