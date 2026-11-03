#!/usr/bin/env python3
"""Verify a real Proxy 3 response timeout is retried through another local proxy."""
import http.client
import json
import os
import pathlib
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
CONFIG = pathlib.Path("/tmp/pg-phase3-retry.json")


def get(host, port, path):
    conn = http.client.HTTPConnection(host, port, timeout=4)
    conn.request("GET", path)
    response = conn.getresponse()
    body = response.read()
    headers = dict(response.getheaders())
    conn.close()
    return response.status, headers, body


def main():
    subprocess.run([str(ROOT / "testbed/run_local.sh"), "stop"], check=False, stdout=subprocess.DEVNULL)
    subprocess.run([str(ROOT / "testbed/run_local.sh"), "start"], check=True)
    cfg = json.loads((ROOT / "config/gateway.local.json").read_text())
    cfg["forwarder"] = os.environ.get("PHASE5_FORWARDER", "epoll")
    cfg["listen"]["port"] = 18001
    cfg["admin"]["port"] = 19001
    cfg["timeouts"]["response_ms"] = 500
    CONFIG.write_text(json.dumps(cfg))
    gateway = subprocess.Popen([str(ROOT / "build/gateway"), "--config", str(CONFIG)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(80):
            if gateway.poll() is not None:
                raise RuntimeError("gateway exited before startup")
            try:
                get("127.0.0.1", 19001, "/health")
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("gateway admin endpoint did not start")
        get("127.0.0.1", 3230, "/fault?delay_ms=700&fail_pct=0")
        # Round-robin reaches proxy IDs 0, 1, then 2. The third request times out on Proxy 3;
        # job-local exclusion sends its retry through a different eligible proxy.
        for index in range(2):
            status, _, _ = get("127.0.0.1", 18001, f"http://test.example/products/{index}")
            assert status == 200, f"warm-up request {index} returned {status}"
        status, headers, _ = get("127.0.0.1", 18001, "http://test.example/products/3")
        assert status == 200, f"retried request returned {status}"
        assert headers.get("X-Gateway-Proxy") == "1", headers
        _, _, proxy_body = get("127.0.0.1", 19001, "/proxies")
        proxies = json.loads(proxy_body)["proxies"]
        _, _, stats_body = get("127.0.0.1", 19001, "/api/stats")
        stats = json.loads(stats_body)
        assert proxies[2]["failure_count"] == 1, proxies[2]
        assert proxies[2]["active_connections"] == 0, proxies[2]
        assert stats["retries"] == 1, stats
        assert proxies[1]["success_count"] == 2, proxies[1]
        print("PHASE3 RETRY PASS: Proxy 3 timed out at 500 ms; retry via proxy 1 returned 200; retries=1")
    finally:
        gateway.terminate()
        gateway.wait(timeout=3)
        subprocess.run([str(ROOT / "testbed/run_local.sh"), "stop"], check=False, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
