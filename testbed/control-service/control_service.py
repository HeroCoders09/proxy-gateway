#!/usr/bin/env python3
"""Local-only, allow-listed control service for the proxy gateway demo."""
import json
import csv
import os
from pathlib import Path
import signal
import subprocess
import sys
from urllib.parse import parse_qs, urlsplit
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import URLError
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[2]
RUN = ROOT / "testbed" / ".run"
PROXY3_PID = RUN / "proxy3.pid"
PORTS = {"proxy1": 3228, "proxy2": 3229, "proxy3": 3230, "proxy4": 3231, "proxy5": 3232}
SCENARIOS = {
    "baseline": {"proxy1": (0, 0), "proxy2": (0, 0), "proxy3": (0, 0), "proxy4": (0, 0), "proxy5": (0, 0)},
    "latency_ladder": {"proxy1": (0, 0), "proxy2": (80, 0), "proxy3": (160, 0), "proxy4": (250, 0), "proxy5": (400, 0)},
    "reliability_gradient": {"proxy1": (0, 0), "proxy2": (0, 5), "proxy3": (0, 15), "proxy4": (0, 25), "proxy5": (0, 40)},
    "combined": {"proxy1": (0, 0), "proxy2": (100, 0), "proxy3": (0, 20), "proxy4": (250, 0), "proxy5": (0, 40)},
}
KNOWN_PROXY_SETTINGS = {
    "proxy1": {"proxy": "proxy1", "delay_ms": 0, "fail_pct": 0.0},
    "proxy2": {"proxy": "proxy2", "delay_ms": 100, "fail_pct": 0.0},
    "proxy3": {"proxy": "proxy3", "delay_ms": 0, "fail_pct": 20.0},
    "proxy4": {"proxy": "proxy4", "delay_ms": 250, "fail_pct": 0.0},
    "proxy5": {"proxy": "proxy5", "delay_ms": 0, "fail_pct": 40.0},
}
BENCHMARK = None
BENCHMARK_LOG = None
LOADGEN = None
LOADGEN_LOG = None


def docker_running(name):
    try:
        p = subprocess.run(["docker", "inspect", "-f", "{{.State.Running}}", name],
                           capture_output=True, text=True, timeout=2)
        return p.returncode == 0 and p.stdout.strip() == "true"
    except (OSError, subprocess.TimeoutExpired):
        return False


def docker_exists(name):
    try:
        p = subprocess.run(["docker", "inspect", name], capture_output=True, timeout=2)
        return p.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def proxy_fault(name, **params):
    query = "&".join(f"{k}={v}" for k, v in params.items())
    with urlopen(f"http://127.0.0.1:{PORTS[name]}/fault?{query}", timeout=2) as response:
        state = json.loads(response.read().decode())
        KNOWN_PROXY_SETTINGS[name] = state
        return state


def restore_proxy3():
    if docker_running("proxy3"):
        subprocess.run(["docker", "start", "proxy3"], check=True, capture_output=True, timeout=10)
    elif local_proxy_pid() is not None:
        local_proxy_signal(signal.SIGCONT)
    elif docker_exists("proxy3"):
        subprocess.run(["docker", "start", "proxy3"], check=True, capture_output=True, timeout=10)
    elif PROXY3_PID.exists():
        raise RuntimeError("Proxy 3 pid file is stale")
    else:
        raise RuntimeError("Proxy 3 is not present in the local or Docker test bed")


def apply_scenario(name):
    if name not in SCENARIOS:
        raise ValueError("unknown scenario")
    restore_proxy3()
    states = {}
    for proxy, (delay, failure) in SCENARIOS[name].items():
        states[proxy] = proxy_fault(proxy, delay_ms=delay, fail_pct=failure)
    return states


def set_proxy_behavior(proxy, delay, failure):
    if proxy not in PORTS:
        raise ValueError("proxy must be one of the five configured test proxies")
    if not 0 <= delay <= 5000 or not 0 <= failure <= 100:
        raise ValueError("delay must be 0..5000 ms and failure rate 0..100 percent")
    if proxy == "proxy3":
        restore_proxy3()
    return proxy_fault(proxy, delay_ms=delay, fail_pct=failure)


def local_proxy_pid():
    if not PROXY3_PID.exists():
        return None
    try:
        pid = int(PROXY3_PID.read_text().strip())
    except ValueError:
        return None
    cmdline = Path(f"/proc/{pid}/cmdline")
    environ = Path(f"/proc/{pid}/environ")
    if (not cmdline.exists() or b"faultproxy.py" not in cmdline.read_bytes() or not environ.exists()
            or b"PROXY_ID=proxy3" not in environ.read_bytes().split(b"\0")):
        return None
    return pid


def local_proxy_signal(sig):
    pid = local_proxy_pid()
    if pid is None:
        raise RuntimeError("Proxy 3 is not running in local test-bed mode")
    os.kill(pid, sig)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def _headers(self):
        origin = self.headers.get("Origin", "")
        self.send_header("Access-Control-Allow-Origin", origin if origin in
                         ("http://127.0.0.1:9000", "http://localhost:9000") else "http://127.0.0.1:9000")
        self.send_header("Vary", "Origin")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Content-Type", "application/json")

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self._headers()
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self._headers()
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        global BENCHMARK_LOG
        if self.path == "/pricelens-summary":
            summary = Path("/tmp/phase6-pricelens-summary.json")
            if not summary.exists():
                self._json(200, {"offers": {}, "requests": 0, "updated_at_unix_ms": 0})
            else:
                try:
                    self._json(200, json.loads(summary.read_text()))
                except (OSError, json.JSONDecodeError):
                    self._json(200, {"offers": {}, "requests": 0, "updated_at_unix_ms": 0})
            return
        if self.path == "/proxy-settings":
            states = {}
            for name in PORTS:
                try:
                    states[name] = proxy_fault(name)
                    states[name]["available"] = True
                except (OSError, URLError, ValueError):
                    states[name] = {**KNOWN_PROXY_SETTINGS[name], "available": False}
            self._json(200, {"proxies": states})
            return
        if self.path != "/benchmark-results":
            if self.path == "/load-status":
                self._json(200,{"status":"running" if LOADGEN is not None and LOADGEN.poll() is None else "idle"}); return
            self._json(404, {"error": "unknown endpoint"}); return
        if BENCHMARK is None:
            self._json(200, {"status": "idle"}); return
        code = BENCHMARK.poll()
        if code is None:
            self._json(200, {"status": "running"}); return
        if BENCHMARK_LOG:
            BENCHMARK_LOG.close(); BENCHMARK_LOG = None
        if code != 0:
            log = Path("/tmp/phase6-benchmark.log")
            self._json(200, {"status": "failed", "error": log.read_text()[-1500:] if log.exists() else "benchmark failed"}); return
        result = ROOT / "bench" / "results" / "phase6_benchmark.csv"
        with result.open(newline="") as f: rows = list(csv.DictReader(f))
        self._json(200, {"status": "complete", "results": rows, "path": "bench/results/phase6_benchmark.csv"})

    def do_POST(self):
        global BENCHMARK, BENCHMARK_LOG, LOADGEN, LOADGEN_LOG
        origin = self.headers.get("Origin", "")
        if origin and origin not in ("http://127.0.0.1:9000", "http://localhost:9000"):
            self._json(403, {"error": "origin not allowed"}); return
        try:
            if self.path == "/kill-proxy3":
                if docker_running("proxy3"):
                    subprocess.run(["docker", "stop", "proxy3"], check=True, capture_output=True, timeout=10)
                elif local_proxy_pid() is not None:
                    local_proxy_signal(signal.SIGSTOP)
                elif docker_exists("proxy3"):
                    pass
                else:
                    local_proxy_signal(signal.SIGSTOP)
                self._json(200, {"message": "Proxy 3 stopped"})
            elif self.path == "/delay-proxy2":
                proxy_fault("proxy2", delay_ms=100)
                self._json(200, {"message": "Proxy 2 delay set to 100 ms"})
            elif self.path.startswith("/proxy-settings?"):
                params = parse_qs(urlsplit(self.path).query, strict_parsing=True)
                proxy = params.get("proxy", [""])[0]
                try:
                    delay = int(params.get("delay_ms", [""])[0])
                    failure = float(params.get("fail_pct", [""])[0])
                    if not failure.is_integer():
                        raise ValueError("failure rate must be a whole percent")
                    state = set_proxy_behavior(proxy, delay, int(failure))
                except (ValueError, KeyError) as exc:
                    self._json(400, {"error": str(exc)[:240]}); return
                self._json(200, {"message": f"{proxy} behavior updated", "proxy": state})
            elif self.path.startswith("/scenario?"):
                params = parse_qs(urlsplit(self.path).query, strict_parsing=True)
                try:
                    name = params.get("name", [""])[0]
                    states = apply_scenario(name)
                except (ValueError, OSError, URLError, subprocess.SubprocessError) as exc:
                    self._json(400, {"error": str(exc)[:240]}); return
                self._json(200, {"message": f"Scenario '{name}' applied", "proxies": states})
            elif self.path == "/restore":
                states = apply_scenario("baseline")
                self._json(200, {"message": "All proxy faults cleared and Proxy 3 resumed", "proxies": states})
            elif self.path == "/run-benchmark":
                if BENCHMARK is not None and BENCHMARK.poll() is None:
                    self._json(409, {"error": "benchmark is already running"}); return
                BENCHMARK_LOG = open("/tmp/phase6-benchmark.log", "w")
                BENCHMARK = subprocess.Popen(["python3", str(ROOT / "bench" / "phase6_benchmark.py")],
                                             cwd=ROOT, stdout=BENCHMARK_LOG, stderr=subprocess.STDOUT, start_new_session=True)
                self._json(202, {"message": "Benchmark started"})
            elif self.path == "/start-load":
                if LOADGEN is not None and LOADGEN.poll() is None:
                    self._json(409,{"error":"load generator is already running"}); return
                LOADGEN_LOG=open("/tmp/phase6-loadgen.log","w")
                LOADGEN=subprocess.Popen(["python3",str(ROOT/"bench"/"load_generator.py"),"--duration-seconds","3600"],
                                         cwd=ROOT,stdout=LOADGEN_LOG,stderr=subprocess.STDOUT,start_new_session=True)
                self._json(202,{"message":"Load generator started (site A and site B)"})
            elif self.path == "/stop-load":
                if LOADGEN is not None and LOADGEN.poll() is None:
                    LOADGEN.terminate()
                    try: LOADGEN.wait(timeout=3)
                    except subprocess.TimeoutExpired: LOADGEN.kill()
                if LOADGEN_LOG:
                    LOADGEN_LOG.close();LOADGEN_LOG=None
                self._json(200,{"message":"Load generator stopped"})
            else:
                self._json(404, {"error": "unknown action"})
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, URLError) as exc:
            self._json(503, {"error": str(exc)[:240]})


if __name__ == "__main__":
    host = os.environ.get("CONTROL_BIND", "127.0.0.1")
    if host not in ("127.0.0.1", "localhost"):
        raise SystemExit("control service may only bind localhost")
    def shutdown(_sig, _frame):
        for process in (LOADGEN, BENCHMARK):
            if process is not None and process.poll() is None:
                try: os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError: pass
        sys.exit(0)
    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    ThreadingHTTPServer((host, int(os.environ.get("CONTROL_PORT", "9100"))), Handler).serve_forever()
