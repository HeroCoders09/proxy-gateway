#!/usr/bin/env bash
# Smoke test: start the test bed and the gateway, send one request through the gateway and check
# status and body (handbook 4.2 task 9, UC1).
# Usage: scripts/smoke.sh [--local]      --local forces the no-Docker test bed
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
trap 'stop_gateway; stop_testbed' EXIT

build_gateway || { echo "SMOKE FAIL: build failed"; exit 1; }
start_testbed "${1:-}" || { echo "SMOKE FAIL: test bed did not start"; exit 1; }
start_gateway || { echo "SMOKE FAIL: gateway did not start"; exit 1; }

URL="http://test.example/products/123"
expected="$(direct_body /products/123)"
status="$(curl -s -o /tmp/smoke_body.$$ -w '%{http_code}' -x "http://127.0.0.1:$GW_PORT" "$URL")"
got="$(cat /tmp/smoke_body.$$)"; rm -f /tmp/smoke_body.$$

[ "$status" = "200" ] || { echo "SMOKE FAIL: expected status 200, got $status"; exit 1; }
[ -n "$expected" ] && [ "$got" = "$expected" ] || { echo "SMOKE FAIL: body differs from the target's own response"; exit 1; }
echo "SMOKE PASS: GET $URL via gateway :$GW_PORT -> $status, body identical to the target's ($(printf %s "$got" | wc -c) bytes)"
