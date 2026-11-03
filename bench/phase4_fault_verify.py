#!/usr/bin/env python3
"""Exercise proxy failure, delay detection and HALF_OPEN recovery in local mode."""
import concurrent.futures
import http.client
import json
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUN = ROOT / "testbed/.run"
GATEWAY_PORT, ADMIN_PORT = 18005, 19005


def connect(host, port, path, timeout=3):
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    conn.request("GET", path)
    response = conn.getresponse()
    body = response.read()
    headers = dict(response.getheaders())
    conn.close()
    return response.status, headers, body


def admin(path):
    return json.loads(connect("127.0.0.1", ADMIN_PORT, path)[2])


def gateway_ready():
    try:
        return connect("127.0.0.1", ADMIN_PORT, "/health")[0] == 200
    except OSError:
        return False


def open_port(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.15):
            return True
    except OSError:
        return False


def wait_until(predicate, timeout, label, interval=0.05):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(interval)
    raise RuntimeError(f"timed out waiting for {label}")


def proxy_state(proxy_id):
    return admin("/proxies")["proxies"][proxy_id]


def request_gateway(index):
    try:
        status, headers, _ = connect("127.0.0.1", GATEWAY_PORT,
                                     f"http://test.example/products/phase4-{index}", timeout=5)
        return status, headers.get("X-Gateway-Proxy")
    except OSError:
        return 599, None


def wait_for_state(proxy_id, expected, timeout):
    return wait_until(lambda: (s if (s := proxy_state(proxy_id))["circuit_state"] == expected else None),
                      timeout, f"proxy {proxy_id} {expected}")


def main():
    # Refuse to interfere with a test bed already running on these fixed local ports.
    occupied = [p for p in (8081, 3128, 3129, 3130, 3228, 3229, 3230, GATEWAY_PORT, ADMIN_PORT) if open_port(p)]
    if occupied:
        raise RuntimeError(f"local test-bed ports already have listeners: {occupied}; stop the test bed you own, then retry")

    bed_started = False
    gateway = None
    config_path = pathlib.Path(tempfile.gettempdir()) / "pg-phase4-health.json"
    cfg = json.loads((ROOT / "config/gateway.local.json").read_text())
    cfg["forwarder"] = os.environ.get("PHASE5_FORWARDER", "epoll")
    cfg["listen"]["port"] = GATEWAY_PORT
    cfg["admin"]["port"] = ADMIN_PORT
    cfg["timeouts"]["response_ms"] = 400
    cfg["circuit"].update({"threshold": 5, "cooldown_ms": 10000, "probe_interval_ms": 2000, "window_size": 20})
    config_path.write_text(json.dumps(cfg))
    try:
        bed_started = True
        subprocess.run([str(ROOT / "testbed/run_local.sh"), "start"], check=True)
        # Make the injected-failure proxy deterministic and healthy before the kill test.
        connect("127.0.0.1", 3230, "/fault?delay_ms=0&fail_pct=0")
        gateway = subprocess.Popen([str(ROOT / "build/gateway"), "--config", str(config_path)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_until(gateway_ready, 5, "gateway startup")
        time.sleep(2.2)  # Let initial health probes settle.

        proxy3_pid = int((RUN / "proxy3.pid").read_text())
        kill_at = time.monotonic()
        os.kill(proxy3_pid, signal.SIGTERM)
        time.sleep(0.1)
        (RUN / "proxy3.pid").unlink(missing_ok=True)
        # Requests keep flowing during detection. Failed transport attempts are retried elsewhere.
        detect_deadline = kill_at + 10.0
        while time.monotonic() < detect_deadline and proxy_state(2)["circuit_state"] != "OPEN":
            request_gateway(0)
            time.sleep(0.04)
        dead = proxy_state(2)
        detect_seconds = time.monotonic() - kill_at
        assert dead["circuit_state"] == "OPEN", dead
        assert detect_seconds <= 10.0, detect_seconds

        before = dead["requests_sent"]
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            outcomes = list(pool.map(request_gateway, range(100)))
        succeeded = sum(status < 500 for status, _ in outcomes)
        assigned_to_dead = sum(proxy_id == "2" for _, proxy_id in outcomes)
        after = proxy_state(2)
        assert succeeded >= 99, f"only {succeeded}/100 requests succeeded"
        assert assigned_to_dead == 0, f"{assigned_to_dead} requests assigned to OPEN proxy 3"
        assert after["requests_sent"] == before, (before, after["requests_sent"])

        # Restart only the test-bed process started by this script. Delay its probe briefly so
        # the HALF_OPEN gate is observable but keep the delay below this config's 400 ms deadline.
        subprocess.run([str(ROOT / "testbed/run_local.sh"), "start"], check=True)
        connect("127.0.0.1", 3230, "/fault?delay_ms=200&fail_pct=0")
        transitions = []
        recovery_started = time.monotonic()
        cooldown_deadline = recovery_started + 15.0
        saw_half_open = False
        while time.monotonic() < cooldown_deadline:
            state = proxy_state(2)["circuit_state"]
            if not transitions or transitions[-1] != state:
                transitions.append(state)
            if state == "HALF_OPEN":
                saw_half_open = True
            if state == "CLOSED" and saw_half_open:
                break
            time.sleep(0.02)
        recovery_seconds = time.monotonic() - recovery_started
        assert saw_half_open and transitions[-1] == "CLOSED", transitions
        connect("127.0.0.1", 3230, "/fault?delay_ms=0&fail_pct=0")

        # Inject and remove a delay on proxy 2; its 1 s health-probe timeout detects 1.5 s delay.
        connect("127.0.0.1", 3229, "/fault?delay_ms=1500&fail_pct=0")
        delay_started = time.monotonic()
        delay_deadline = delay_started + 10.0
        while time.monotonic() < delay_deadline and proxy_state(1)["circuit_state"] != "OPEN":
            request_gateway(0)  # Traffic outcomes share the rolling health and breaker stream.
            time.sleep(0.05)
        delay_detect_seconds = time.monotonic() - delay_started
        assert proxy_state(1)["circuit_state"] == "OPEN", proxy_state(1)
        connect("127.0.0.1", 3229, "/fault?delay_ms=0&fail_pct=0")
        delay_recovery_started = time.monotonic()
        wait_for_state(1, "CLOSED", 15.0)
        delay_recovery_seconds = time.monotonic() - delay_recovery_started

        summary = ("# Phase 4 fault injection results\n\n"
                   "Local test bed, three proxies; gateway response timeout 400 ms; probe cadence 2 s; "
                   "probe deadline 1 s; circuit threshold 5 and cooldown 10 s.\n\n"
                   f"- Proxy 3 kill detection: {detect_seconds:.2f} s (limit 10 s); circuit `{dead['circuit_state']}`.\n"
                   f"- Requests during one-proxy outage: {succeeded}/100 successful by D-010 "
                   f"({succeeded}%), assigned to OPEN Proxy 3: {assigned_to_dead}.\n"
                   f"- Proxy 3 restart recovery: {recovery_seconds:.2f} s; observed states: "
                   f"`{' -> '.join(transitions)}`.\n"
                   f"- Proxy 2 with 1,500 ms delay detection: {delay_detect_seconds:.2f} s. "
                   f"After removing delay it returned to CLOSED in {delay_recovery_seconds:.2f} s.\n")
        (ROOT / "bench/results/phase4_fault_injection.md").write_text(summary)
        print("PHASE4 PASS: " + summary.replace("\n", " "))
    finally:
        if gateway is not None:
            gateway.terminate()
            gateway.wait(timeout=4)
        if bed_started:
            subprocess.run([str(ROOT / "testbed/run_local.sh"), "stop"], check=False,
                           stdout=subprocess.DEVNULL)
        config_path.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
