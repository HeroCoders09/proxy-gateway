#!/usr/bin/env python3
"""Compare Phase 3 routing policies against the local test bed; standard library only."""
import argparse
import concurrent.futures
import http.client
import math
import os
import pathlib
import socket
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
POLICIES = ("random", "roundrobin", "leastconn", "latency", "weighted")


def percentile(values, pct):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(pct * len(ordered)) - 1)] if ordered else 0.0


def wait_port(port, process):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("gateway exited before opening port 8000")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("timed out waiting for gateway port")


def one_request(index):
    started = time.perf_counter()
    conn = http.client.HTTPConnection("127.0.0.1", 8000, timeout=15)
    try:
        conn.request("GET", f"http://test.example/products/{index}", headers={"Connection": "close"})
        response = conn.getresponse()
        response.read()
        return (time.perf_counter() - started) * 1000, response.status < 500
    except OSError:
        return (time.perf_counter() - started) * 1000, False
    finally:
        conn.close()


def run_policy(policy, count, concurrency):
    subprocess.run([str(ROOT / "testbed/run_local.sh"), "stop"], check=False, stdout=subprocess.DEVNULL)
    env = dict(os.environ, FAIL_SEED="43031")
    subprocess.run([str(ROOT / "testbed/run_local.sh"), "start"], check=True, env=env)
    gateway = subprocess.Popen([str(ROOT / "build/gateway"), "--config", str(ROOT / "config/gateway.local.json"),
                                "--policy", policy], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_port(8000, gateway)
        started = time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
            values = list(pool.map(one_request, range(count)))
        elapsed = time.perf_counter() - started
        latencies = [value[0] for value in values]
        successes = sum(value[1] for value in values)
        return {
            "policy": policy,
            "requests": count,
            "concurrency": concurrency,
            "p50_ms": percentile(latencies, 0.50),
            "p95_ms": percentile(latencies, 0.95),
            "p99_ms": percentile(latencies, 0.99),
            "throughput_rps": count / elapsed if elapsed else 0.0,
            "success_rate": successes / count if count else 0.0,
        }
    finally:
        gateway.terminate()
        try:
            gateway.wait(timeout=3)
        except subprocess.TimeoutExpired:
            gateway.kill()
            gateway.wait()
        subprocess.run([str(ROOT / "testbed/run_local.sh"), "stop"], check=False, stdout=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--requests", type=int, default=100, help="requests per policy (default: 100)")
    parser.add_argument("--concurrency", type=int, default=8, help="parallel client connections (default: 8)")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "bench/results/phase3_routing.csv")
    args = parser.parse_args()
    if args.requests < 1 or args.concurrency < 1:
        parser.error("--requests and --concurrency must be positive")
    rows = [run_policy(policy, args.requests, args.concurrency) for policy in POLICIES]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    columns = ("policy", "requests", "concurrency", "p50_ms", "p95_ms", "p99_ms", "throughput_rps", "success_rate")
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        stream.write(",".join(columns) + "\n")
        for row in rows:
            stream.write(",".join(str(row[column]) for column in columns) + "\n")
    print("policy,p50_ms,p95_ms,p99_ms,throughput_rps,success_rate")
    for row in rows:
        print("{policy},{p50_ms:.2f},{p95_ms:.2f},{p99_ms:.2f},{throughput_rps:.2f},{success_rate:.3f}".format(**row))
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
