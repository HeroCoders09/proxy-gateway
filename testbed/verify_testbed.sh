#!/usr/bin/env bash
# Verifies the three proxy behaviours with DIRECT requests (Phase 1 checklist item):
#   proxy1 normal, proxy2 adds ~100 ms, proxy3 fails ~20 percent of requests.
# Usage: testbed/verify_testbed.sh [local|docker]    (default: local)
# The URL uses test.example, which the proxies resolve themselves, so both modes use the same ports.
set -u
MODE="${1:-local}"
N="${N:-200}"
URL="http://test.example/products/123"
fail=0

req() {  # proxy_port -> prints "status time_ms"
  curl -s -o /dev/null -x "http://127.0.0.1:$1" -w '%{http_code} %{time_total}\n' --max-time 10 "$URL"
}

avg_ms() {  # proxy_port, count
  local port="$1" n="$2" total=0 ok=0
  for _ in $(seq 1 "$n"); do
    read -r code t < <(req "$port")
    if [ "$code" = "200" ]; then
      total=$(awk -v a="$total" -v b="$t" 'BEGIN{print a+b}')
      ok=$((ok+1))
    fi
  done
  awk -v t="$total" -v n="$ok" 'BEGIN{ if(n>0) printf "%.1f", t/n*1000; else print "nan" }'
}

fail_pct() {  # proxy_port, count
  local port="$1" n="$2" bad=0
  for _ in $(seq 1 "$n"); do
    read -r code _ < <(req "$port")
    [ "$code" = "200" ] || bad=$((bad+1))
  done
  awk -v b="$bad" -v n="$n" 'BEGIN{printf "%.1f", b/n*100}'
}

for p in 3128 3129 3130; do
  (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null || { echo "FAIL nothing is listening on proxy port $p: start the test bed first (testbed/run_local.sh start, or docker compose up)"; exit 2; }
done
a1=$(avg_ms 3128 50); a2=$(avg_ms 3129 50)
echo "proxy1 avg latency: ${a1} ms (normal)"
echo "proxy2 avg latency: ${a2} ms (expected about proxy1 + 100 ms)"
diff=$(awk -v a="$a2" -v b="$a1" 'BEGIN{printf "%.1f", a-b}')
echo "difference proxy2 - proxy1: ${diff} ms"
awk -v d="$diff" 'BEGIN{exit !(d>=80 && d<=160)}' && echo "PASS proxy2 delay is about 100 ms" || { echo "FAIL proxy2 delay not in 80..160 ms"; fail=1; }

f1=$(fail_pct 3128 "$N"); f3=$(fail_pct 3130 "$N")
echo "proxy1 failures: ${f1} % of $N"
echo "proxy3 failures: ${f3} % of $N (expected about 20 %)"
awk -v f="$f1" 'BEGIN{exit !(f==0)}' && echo "PASS proxy1 never fails" || { echo "FAIL proxy1 failed requests"; fail=1; }
awk -v f="$f3" 'BEGIN{exit !(f>=10 && f<=30)}' && echo "PASS proxy3 fails about 20 percent" || { echo "FAIL proxy3 failure rate not in 10..30 %"; fail=1; }
exit $fail
