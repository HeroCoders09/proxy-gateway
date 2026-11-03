#!/usr/bin/env python3
"""Small forward proxy with injectable faults, used as the three test-bed proxy nodes.

Standard library only. It accepts proxy-style requests with absolute URLs
("GET http://test.example/products/1 HTTP/1.1") and HTTP CONNECT, relays them to the target,
and can misbehave on purpose:

  DELAY_MS    extra delay added before relaying every request          (Proxy 2: 100)
  FAIL_PCT    percent of requests answered with 502 instead of relayed (Proxy 3: 20)
  PORT        proxy listen port (default 3128)
  CONTROL_PORT  if set, a second port that changes faults at run time:
                GET /fault?delay_ms=100&fail_pct=20   (either parameter is optional)
                GET /fault                            (show current values)
  HOST_OVERRIDES  "name=host:port,name2=host:port"  map target names to real addresses,
                so the same proxy runs without Docker (see testbed/run_local.sh)
  FAIL_SEED   integer; makes the failure pattern reproducible (default: random)

Why not Squid/tinyproxy + tc netem: those need extra privileges (NET_ADMIN) and a different
config per fault. These env vars give the same three behaviours on any machine, and the
control port gives Phase 6 a way to add or remove delay without root. See docs/DECISIONS.md D-011.

Only metadata is logged (never headers, bodies or credentials).
"""
import http.client
import os
import random
import select
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

PROXY_ID = os.environ.get("PROXY_ID", "proxy")
_state = {
    "delay_ms": int(os.environ.get("DELAY_MS", "0")),
    "fail_pct": float(os.environ.get("FAIL_PCT", "0")),
}
_state_lock = threading.Lock()
_rng = random.Random(int(os.environ["FAIL_SEED"])) if os.environ.get("FAIL_SEED") else random.Random()
_rng_lock = threading.Lock()

OVERRIDES = {}
for item in filter(None, os.environ.get("HOST_OVERRIDES", "").split(",")):
    name, _, addr = item.partition("=")
    host, _, port = addr.partition(":")
    OVERRIDES[name.strip().lower()] = (host, int(port or 80))

HOP_BY_HOP = {"connection", "keep-alive", "proxy-authenticate", "proxy-authorization", "proxy-connection",
              "te", "trailer", "transfer-encoding", "upgrade"}


def current():
    with _state_lock:
        return _state["delay_ms"], _state["fail_pct"]


def should_fail(pct: float) -> bool:
    if pct <= 0:
        return False
    with _rng_lock:
        return _rng.random() * 100.0 < pct


def resolve(host: str, port: int):
    return OVERRIDES.get(host.lower(), (host, port))


class ProxyHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # keep-alive towards the gateway (Phase 5 reuse)
    server_version = "faultproxy/1"

    def setup(self):
        super().setup()
        # Avoid Nagle delaying the small response-body write behind an unacknowledged header
        # segment on persistent gateway-to-proxy connections.
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def log_message(self, fmt, *args):
        pass

    def _reply(self, status, text, extra=None):
        body = (text + "\n").encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Proxy-Id", PROXY_ID)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _apply_faults(self) -> bool:
        """Returns True if the request was already answered with an injected failure."""
        delay_ms, fail_pct = current()
        if delay_ms > 0:
            time.sleep(delay_ms / 1000.0)
        if should_fail(fail_pct):
            self._reply(502, "injected failure")
            return True
        return False

    def _relay(self):
        parts = urlsplit(self.path)
        if parts.scheme != "http" or not parts.hostname:
            return self._reply(400, "absolute http:// URL required")
        if self._apply_faults():
            return
        host, port = resolve(parts.hostname, parts.port or 80)
        target = parts.path or "/"
        if parts.query:
            target += "?" + parts.query

        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None
        headers = {k: v for k, v in self.headers.items() if k.lower() not in HOP_BY_HOP}
        headers["Host"] = parts.netloc
        try:
            conn = http.client.HTTPConnection(host, port, timeout=10)
            conn.request(self.command, target, body=body, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
            self.send_response(resp.status, resp.reason)
            for k, v in resp.getheaders():
                if k.lower() in HOP_BY_HOP or k.lower() == "content-length":
                    continue
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("X-Proxy-Id", PROXY_ID)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(data)
            conn.close()
        except (OSError, http.client.HTTPException):
            self._reply(502, "cannot reach target")

    do_GET = do_POST = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = do_HEAD = _relay

    def do_CONNECT(self):
        host, _, port = self.path.partition(":")
        if self._apply_faults():
            return
        try:
            upstream = socket.create_connection(resolve(host, int(port or 443)), timeout=5)
        except OSError:
            return self._reply(502, "cannot reach target")
        self.send_response(200, "Connection Established")
        self.end_headers()
        self.close_connection = True
        socks = [self.connection, upstream]
        try:
            while True:
                readable, _, _ = select.select(socks, [], [], 30)
                if not readable:
                    break
                for s in readable:
                    data = s.recv(65536)
                    if not data:
                        return
                    (upstream if s is self.connection else self.connection).sendall(data)
        except OSError:
            pass
        finally:
            upstream.close()


class ControlHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        path, _, query = self.path.partition("?")
        if path != "/fault":
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
        with _state_lock:
            try:
                if "delay_ms" in params:
                    _state["delay_ms"] = max(0, int(params["delay_ms"]))
                if "fail_pct" in params:
                    _state["fail_pct"] = min(100.0, max(0.0, float(params["fail_pct"])))
            except ValueError:
                pass
            body = ('{"proxy":"%s","delay_ms":%d,"fail_pct":%s}' % (PROXY_ID, _state["delay_ms"], _state["fail_pct"])).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 2048


if __name__ == "__main__":
    bind = os.environ.get("BIND", "0.0.0.0")
    cport = os.environ.get("CONTROL_PORT")
    if cport:
        # Test-bed only: the control port is not published outside the host (see docker-compose.yml).
        threading.Thread(target=Server((bind, int(cport)), ControlHandler).serve_forever, daemon=True).start()
    Server((bind, int(os.environ.get("PORT", "3128"))), ProxyHandler).serve_forever()
