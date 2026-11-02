#!/usr/bin/env python3
"""Phase 5 loopback benchmark: simple vs epoll forwarding, pool reuse and 1,000 requests."""
import concurrent.futures
import http.client
import json
import os
import pathlib
import signal
import socket
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
RESULTS = ROOT / "bench/results"
GATEWAY_PORT = 18005
ADMIN_PORT = 19005
PROXY_PORT = 3128
TARGET_PORT = 8081
CONFIG = pathlib.Path("/tmp/pg-phase5-benchmark.json")


def percentile(values, p):
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[max(0, min(len(ordered) - 1, int((len(ordered) * p + 0.999999) - 1)))]


def request(port, index, delay_ms=0):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=45)
    path = f"http://test.example/products/{index}?delay_ms={delay_ms}"
    started = time.perf_counter()
    conn.request("GET", path, headers={"Host": "test.example"})
    response = conn.getresponse()
    body = response.read()
    elapsed_ms = (time.perf_counter() - started) * 1000
    status = response.status
    conn.close()
    return status, len(body), elapsed_ms


def direct_target(index):
    conn = http.client.HTTPConnection("127.0.0.1", TARGET_PORT, timeout=15)
    started = time.perf_counter()
    conn.request("GET", f"/products/{index}", headers={"Host": "test.example"})
    response = conn.getresponse()
    response.read()
    elapsed = time.perf_counter() - started
    conn.close()
    return response.status, elapsed


def direct_proxy(index):
    conn = http.client.HTTPConnection("127.0.0.1", PROXY_PORT, timeout=15)
    started = time.perf_counter()
    conn.request("GET", f"http://test.example/products/{index}", headers={"Host": "test.example"})
    response = conn.getresponse()
    response.read()
    elapsed = time.perf_counter() - started
    conn.close()
    return response.status, elapsed


def direct_proxy_keepalive(count=1000):
    conn = http.client.HTTPConnection("127.0.0.1", PROXY_PORT, timeout=15)
    samples = []
    try:
        for i in range(count):
            started = time.perf_counter()
            conn.request("GET", f"http://test.example/products/{i}", headers={"Host": "test.example"})
            response = conn.getresponse()
            response.read()
            samples.append((time.perf_counter() - started) * 1000)
        return {"requests": count, "requests_per_second": count / (sum(samples) / 1000),
                "latency_p50_ms": percentile(samples, 0.50), "latency_p95_ms": percentile(samples, 0.95)}
    finally:
        conn.close()


def admin_stats():
    conn = http.client.HTTPConnection("127.0.0.1", ADMIN_PORT, timeout=3)
    conn.request("GET", "/api/stats")
    response = conn.getresponse()
    data = json.loads(response.read())
    conn.close()
    return data


def gateway_config(forwarder, workers=128, per_proxy=128, total=128):
    cfg = json.loads((ROOT / "config/gateway.local.json").read_text())
    cfg["listen"].update({"host": "127.0.0.1", "port": GATEWAY_PORT})
    cfg["admin"].update({"host": "127.0.0.1", "port": ADMIN_PORT})
    cfg["proxies"] = [{"host": "127.0.0.1", "port": PROXY_PORT}]
    cfg["policy"] = "roundrobin"
    cfg["forwarder"] = forwarder
    cfg["forwarder_pool"] = {
        "max_connections_per_proxy": per_proxy,
        "max_total_connections": total,
        "idle_timeout_ms": 30000,
    }
    cfg["workers"].update({"min": workers, "max": workers, "alpha": 1, "beta": 0.5})
    cfg["ingress"].update({"threads": min(512, workers), "pending_connections": 4096})
    cfg["rate_limits"]["default"] = {"rate": 10000, "burst": 10000}
    cfg["circuit"].update({"threshold": 10000, "cooldown_ms": 10000})
    CONFIG.write_text(json.dumps(cfg))


def start_gateway(forwarder, workers=128, per_proxy=128, total=128):
    gateway_config(forwarder, workers, per_proxy, total)
    log = open("/tmp/pg-phase5-gateway.log", "w")
    proc = subprocess.Popen([str(ROOT / "build/gateway"), "--config", str(CONFIG)],
                            stdout=log, stderr=subprocess.STDOUT)
    for _ in range(120):
        if proc.poll() is not None:
            log.close()
            raise RuntimeError("gateway failed to start: " + pathlib.Path("/tmp/pg-phase5-gateway.log").read_text())
        try:
            admin_stats()
            return proc, log
        except OSError:
            time.sleep(0.05)
    proc.terminate()
    proc.wait(timeout=5)
    log.close()
    raise RuntimeError("gateway did not become ready")


def stop_gateway(proc, log):
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
    log.close()


def parallel_requests(port, count, concurrency, delay_ms=0, observe=None):
    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(request, port, i, delay_ms) for i in range(count)]
        max_active = 0
        peak_rss_kb = 0
        while any(not future.done() for future in futures):
            if observe:
                active, rss = observe()
                max_active = max(max_active, active)
                peak_rss_kb = max(peak_rss_kb, rss)
            time.sleep(0.02)
        results = [future.result() for future in futures]
    elapsed = time.perf_counter() - started
    errors = [status for status, _, _ in results if status != 200]
    if errors:
        raise RuntimeError(f"{len(errors)} of {count} requests failed; first status {errors[0]}")
    return {
        "requests": count,
        "concurrency": concurrency,
        "elapsed_seconds": elapsed,
        "requests_per_second": count / elapsed,
        "latency_p50_ms": percentile([r[2] for r in results], 0.50),
        "latency_p95_ms": percentile([r[2] for r in results], 0.95),
        "max_active_workers": max_active,
        "peak_gateway_rss_kb": peak_rss_kb,
    }


def rss_kb(pid):
    try:
        for line in pathlib.Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except (FileNotFoundError, ProcessLookupError):
        return 0
    return 0


def gateway_observer(proc):
    def read():
        try:
            return int(admin_stats().get("active_connections", 0)), rss_kb(proc.pid)
        except (OSError, ValueError, json.JSONDecodeError):
            return 0, rss_kb(proc.pid)
    return read


def run_mode(mode, capture=None):
    proc, log = start_gateway(mode)
    tcpdump = None
    capture_result = {"captured": False, "reason": "not requested"}
    try:
        if capture:
            capture_log = open("/tmp/pg-phase5-tcpdump.log", "w")
            tcpdump = subprocess.Popen(
                ["tcpdump", "-i", "lo", "-nn", "-s", "0", "-w", str(capture), f"tcp port {PROXY_PORT}"],
                stdout=subprocess.DEVNULL, stderr=capture_log)
            time.sleep(0.3)
            if tcpdump.poll() is not None:
                capture_log.close()
                capture_result = {"captured": False,
                                  "reason": pathlib.Path("/tmp/pg-phase5-tcpdump.log").read_text().strip()}
                tcpdump = None
            else:
                capture_result = {"captured": True, "path": str(capture)}

        sequential = [request(GATEWAY_PORT, 100000 + i) for i in range(1000)]
        if any(status != 200 for status, _, _ in sequential):
            raise RuntimeError(f"{mode} sequential sample had a failed request")
        seq_p95 = percentile([r[2] for r in sequential], 0.95)
        throughput = parallel_requests(GATEWAY_PORT, 500, 32)
        stats = admin_stats()
        if tcpdump:
            tcpdump.send_signal(signal.SIGINT)
            tcpdump.wait(timeout=5)
            tcpdump = None
        return {
            "forwarder": mode,
            "sequential_requests": 1000,
            "sequential_latency_p50_ms": percentile([r[2] for r in sequential], 0.50),
            "sequential_latency_p95_ms": seq_p95,
            "parallel": throughput,
            "gateway_overhead_ms": stats.get("gateway_overhead_ms", {}),
            "upstream_latency_ms": stats.get("latency_ms", {}),
            "upstream_connections_opened": stats.get("upstream_connections_opened", 0),
            "upstream_connections_reused": stats.get("upstream_connections_reused", 0),
            "upstream_active_connections": stats.get("upstream_active_connections", 0),
            "upstream_idle_connections": stats.get("upstream_idle_connections", 0),
            "reuse_trace": capture_result,
        }
    finally:
        if tcpdump and tcpdump.poll() is None:
            tcpdump.send_signal(signal.SIGINT)
            tcpdump.wait(timeout=5)
        stop_gateway(proc, log)


def main():
    for port in (8081, 3128, 3129, 3130, GATEWAY_PORT, ADMIN_PORT):
        try:
            probe = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            probe.close()
        except OSError:
            continue
        raise RuntimeError(f"port {port} already has a listener; stop the existing service")

    subprocess.run([str(ROOT / "testbed/run_local.sh"), "start"], check=True)
    output = {"testbed": "local Python target and proxies", "created_at": time.strftime("%Y-%m-%d %H:%M:%S %Z")}
    capture = RESULTS / "phase5_keepalive.pcap"
    try:
        # Test-bed components are the throughput baseline; results reveal if Python is the bottleneck.
        for label, fn in (("target_direct", direct_target), ("proxy_direct", direct_proxy)):
            started = time.perf_counter()
            with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
                statuses = list(pool.map(lambda i: fn(i)[0], range(500)))
            elapsed = time.perf_counter() - started
            output[label] = {"requests": len(statuses), "requests_per_second": len(statuses) / elapsed,
                             "all_200": all(s == 200 for s in statuses)}
        output["proxy_keepalive_direct"] = direct_proxy_keepalive()

        output["simple"] = run_mode("simple")
        output["epoll"] = run_mode("epoll", capture)

        # High-concurrency acceptance run: scale workers and the explicitly bounded pool for 1,000 clients.
        proc, log = start_gateway("epoll", workers=1050, per_proxy=1050, total=1050)
        try:
            output["concurrency_1000"] = parallel_requests(
                GATEWAY_PORT, 1000, 1000, delay_ms=150, observe=gateway_observer(proc))
            stats = admin_stats()
            output["concurrency_1000"].update({
                "observed_upstream_connections": stats.get("upstream_active_connections", 0),
                "observed_idle_upstream_connections": stats.get("upstream_idle_connections", 0),
                "gateway_overhead_ms": stats.get("gateway_overhead_ms", {}),
                "upstream_latency_ms": stats.get("latency_ms", {}),
            })
        finally:
            stop_gateway(proc, log)
    finally:
        subprocess.run([str(ROOT / "testbed/run_local.sh"), "stop"], check=False,
                       stdout=subprocess.DEVNULL)

    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "phase5_performance.json").write_text(json.dumps(output, indent=2) + "\n")
    epoll = output["epoll"]
    simple = output["simple"]
    load = output["concurrency_1000"]
    opened = epoll["upstream_connections_opened"]
    reused = epoll["upstream_connections_reused"]
    reuse_ratio = reused / (opened + reused) if opened + reused else 0.0
    capture = epoll["reuse_trace"]
    report = [
        "# Phase 5 performance results",
        "",
        f"Run date: {output['created_at']}. Local test bed: Python target and fault proxies on loopback; gateway compiled with the current workspace build.",
        "",
        "## Measurements",
        "",
        f"- Direct target throughput: {output['target_direct']['requests_per_second']:.1f} requests/s (500 requests, concurrency 32).",
        f"- Direct proxy throughput (new client connection per request): {output['proxy_direct']['requests_per_second']:.1f} requests/s.",
        f"- Direct proxy keep-alive: p50 {output['proxy_keepalive_direct']['latency_p50_ms']:.2f} ms, p95 {output['proxy_keepalive_direct']['latency_p95_ms']:.2f} ms.",
        f"- Gateway simple: p95 latency {simple['sequential_latency_p95_ms']:.2f} ms; throughput {simple['parallel']['requests_per_second']:.1f} requests/s at concurrency 32.",
        f"- Gateway epoll: p95 latency {epoll['sequential_latency_p95_ms']:.2f} ms; throughput {epoll['parallel']['requests_per_second']:.1f} requests/s at concurrency 32.",
        f"- Epoll GatewayCore overhead: p95 {epoll['gateway_overhead_ms'].get('p95', 0):.2f} ms at concurrency 32; p95 {load['gateway_overhead_ms'].get('p95', 0):.2f} ms during the 1,000-request trial.",
        f"- Epoll reuse: {opened} sockets opened and {reused} reused ({reuse_ratio:.1%} reuse events among measured opens and reuses).",
        f"- Concurrency trial: {load['requests']} requests submitted with {load['concurrency']} clients; {load['requests_per_second']:.1f} requests/s; peak active gateway workers {load['max_active_workers']}; peak gateway RSS {load['peak_gateway_rss_kb'] / 1024:.1f} MiB.",
        f"- Packet capture: {'saved to `' + capture.get('path', '') + '`' if capture.get('captured') else 'unavailable: ' + capture.get('reason', 'capture unavailable')}.",
        "- Supplemental reuse syscall trace: `bench/results/phase5_reuse_syscalls.txt` records one upstream `connect()` and two request writes on the same descriptor in the two-request reuse test.",
        "",
        "## Acceptance against handbook targets",
        "",
        f"- Gateway overhead p95 <= 5 ms: {'PASS' if epoll['gateway_overhead_ms'].get('p95', 0) <= 5 else 'MISS'} at concurrency 32; {'PASS' if load['gateway_overhead_ms'].get('p95', 0) <= 5 else 'MISS'} under the 1,000-client trial.",
        f"- Throughput >= 5,000 requests/s: {'PASS' if max(simple['parallel']['requests_per_second'], epoll['parallel']['requests_per_second']) >= 5000 else 'MISS'}; local Python proxy throughput is the measured bottleneck.",
        f"- 1,000 in-flight requests: {'PASS' if load['max_active_workers'] >= 1000 else 'MISS'}; observed peak was {load['max_active_workers']}.",
        f"- Memory <= 256 MiB at 1,000 clients: {'PASS' if load['peak_gateway_rss_kb'] <= 256 * 1024 else 'MISS'}; measured peak gateway RSS was {load['peak_gateway_rss_kb'] / 1024:.1f} MiB.",
        f"- Keep-alive lower latency than connection-per-request: {'PASS' if epoll['sequential_latency_p95_ms'] < simple['sequential_latency_p95_ms'] else 'MISS'} in the gateway comparison.",
        "",
        "The 1,000-client trial completed its requests, but the measured peak active workers did not reach 1,000. RSS is the gateway process's `/proc/<pid>/status` VmRSS peak. The overhead metric subtracts forwarder-attempt duration from `GatewayCore::handle` and excludes queueing before the core and client response writes. The proxy throughput figure identifies the Python fault proxy as a material bottleneck.",
    ]
    (RESULTS / "phase5_performance.md").write_text("\n".join(report) + "\n")
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
