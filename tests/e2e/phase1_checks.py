#!/usr/bin/env python3
"""End-to-end checks for the Phase 1 done checklist. Run by tests/e2e/run_phase1_acceptance.sh
against a running gateway (127.0.0.1:8000, admin :9000) and test bed. Prints PASS/FAIL lines and
exits non-zero if anything failed. Standard library only.

Environment: GW_PID (gateway process id, for the descriptor-leak check), TARGET_PORT (direct port).
"""
import json
import os
import socket
import struct
import sys
import time
import urllib.request

GW = ("127.0.0.1", 8000)
ADMIN = "http://127.0.0.1:9000"
TARGET_PORT = int(os.environ.get("TARGET_PORT", "8081"))
GW_PID = int(os.environ["GW_PID"]) if os.environ.get("GW_PID") else None
failures = 0


def check(ok, msg, extra=""):
    global failures
    print(("PASS " if ok else "FAIL ") + msg + (f"  [{extra}]" if extra else ""))
    if not ok:
        failures += 1


def raw_exchange(payload: bytes, timeout=5.0, shutdown_write=False):
    """Send raw bytes to the gateway, return everything it answers."""
    with socket.create_connection(GW, timeout=timeout) as s:
        s.sendall(payload)
        if shutdown_write:
            s.shutdown(socket.SHUT_WR)
        chunks = []
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                chunks.append(d)
        except socket.timeout:
            pass
        except ConnectionResetError:
            pass  # a reset after the reply must not discard what was already received
        return b"".join(chunks)


def via_gateway(url: str, method="GET"):
    host = url.split("/")[2]
    req = f"{method} {url} HTTP/1.1\r\nHost: {host}\r\n\r\n".encode()
    resp = raw_exchange(req)
    head, _, body = resp.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1]) if head.startswith(b"HTTP/") else 0
    return status, head, body


def direct(path: str) -> bytes:
    req = urllib.request.Request(f"http://127.0.0.1:{TARGET_PORT}{path}", headers={"Host": "test.example"})
    return urllib.request.urlopen(req, timeout=5).read()


def open_fds(pid):
    return len(os.listdir(f"/proc/{pid}/fd"))


def gateway_alive():
    try:
        s, _, b = via_gateway("http://test.example/products/1")
        return s == 200 and b == direct("/products/1")
    except OSError:
        return False


# 1. UC1: the example request from the handbook
status, head, body = via_gateway("http://test.example/products/123")
check(status == 200 and body == direct("/products/123"), "UC1: GET http://test.example/products/123 returns 200 and the exact body")
check(b"X-Gateway-Proxy" in head, "response names the proxy that served it (X-Gateway-Proxy header)")

# 2. 1,000 sequential requests with correct bodies
fds_before = open_fds(GW_PID) if GW_PID else None
t0 = time.time()
bad = 0
for i in range(1, 1001):
    s, _, b = via_gateway(f"http://test.example/products/{i}")
    if s != 200 or b != direct(f"/products/{i}"):
        bad += 1
elapsed = time.time() - t0
check(bad == 0, "1,000 sequential requests return correct bodies", f"{1000 - bad}/1000 correct, {elapsed:.1f} s including direct comparisons")
if GW_PID:
    time.sleep(0.3)
    fds_after = open_fds(GW_PID)
    check(fds_after <= fds_before + 2, "no file-descriptor leak after 1,000 requests", f"open fds before {fds_before}, after {fds_after}")

# 3. malformed requests -> 400, gateway keeps running
cases = {
    "garbage bytes": b"\x00\x01\x02 not http at all\r\n\r\n",
    "no absolute URL": b"GET /products/1 HTTP/1.1\r\nHost: test.example\r\n\r\n",
    "bad version": b"GET http://test.example/ HTTP/9.9\r\n\r\n",
    "bad method": b"BREW http://test.example/ HTTP/1.1\r\n\r\n",
    "https URL": b"GET https://test.example/ HTTP/1.1\r\n\r\n",
    "bad header line": b"GET http://test.example/ HTTP/1.1\r\nthis is not a header\r\n\r\n",
    "conflicting Content-Length": b"POST http://test.example/ HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
    "huge header": b"GET http://test.example/ HTTP/1.1\r\nX: " + b"a" * 40000 + b"\r\n\r\n",
    "truncated then closed": b"GET http://test.example/pro",
}
for name, payload in cases.items():
    resp = raw_exchange(payload, shutdown_write=(name == "truncated then closed"))
    check(resp.startswith(b"HTTP/1.1 400"), f"malformed request ({name}) returns HTTP 400")
check(gateway_alive(), "gateway still serves correct responses after all malformed requests")

# 4. peer resets mid-request must not crash the gateway
for _ in range(20):
    s = socket.create_connection(GW)
    s.sendall(b"GET http://test.example/products/9 HTTP/1.1\r\nHo")
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))  # close sends RST
    s.close()
time.sleep(0.2)
check(gateway_alive(), "gateway survives 20 client connections reset mid-request")

# 5. live admin API: documented shapes
def admin(path):
    with urllib.request.urlopen(ADMIN + path, timeout=3) as r:
        return r.status, r.read().decode(), r.headers.get("Content-Type", "")

st, txt, ctype = admin("/health")
h = json.loads(txt)
check(st == 200 and h.get("status") == "ok" and "uptime_seconds" in h, "admin /health returns the documented shape")
st, txt, _ = admin("/proxies")
p = json.loads(txt)
need = {"id", "host", "port", "healthy", "rtt_ms", "success_count", "failure_count", "consecutive_failures",
        "success_rate", "active_connections", "circuit_state", "circuit_open_count", "last_probe_unix_ms"}
check(st == 200 and len(p["proxies"]) == 3 and all(need <= set(x) for x in p["proxies"]), "admin /proxies lists 3 proxies with all documented fields")
st, txt, _ = admin("/api/stats")
a = json.loads(txt)
check(st == 200 and a["requests_total"] >= 1000 and a["requests_failed"] == 0, "admin /api/stats shows real counters", f"requests_total={a['requests_total']}")
st, txt, _ = admin("/metrics")
check("requests_total" in txt and "requests_successful" in txt and "timeouts" in txt and "bad_requests" in txt, "admin /metrics is Prometheus text with the Phase 1 counters")
m = {l.split()[0]: int(l.split()[1]) for l in txt.splitlines() if l and not l.startswith("#") and "{" not in l and l.split()[1].isdigit()}
check(m.get("bad_requests", 0) >= len(cases), "bad_requests counter counted the malformed requests", f"bad_requests={m.get('bad_requests')}")

# 6. a request for a missing page passes the target's 404 through unchanged
s, _, _ = via_gateway("http://test.example/nope")
check(s == 404, "target 404 is passed through to the client")

sys.exit(1 if failures else 0)
