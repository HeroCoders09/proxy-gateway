#!/usr/bin/env python3
"""Controlled target server for the proxy-gateway test bed (Phase 1).

Standard library only. Behaviour:

  GET /products/<id>   deterministic page; the body contains the id, so tests can check it
  GET /health          small JSON document used by probes (Phase 4)
  GET /stats           JSON request counts per Host header (Phase 2 rate-limit load test)
  any path             "?delay_ms=N" adds N ms of delay for that request

Environment:
  PORT         listen port (default 80)
  DELAY_MS     default delay applied to every /products/ request (default 0)
  BIND         bind address (default 0.0.0.0)

The Host header is recorded per request, so one server can stand in for several sites
(test.example, site-a.example, site-b.example): see /stats. No response bodies or request
headers are logged.
"""
import json
import os
import threading
import time
from urllib.parse import unquote
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_DELAY_MS = int(os.environ.get("DELAY_MS", "0"))

_lock = threading.Lock()
_counts = {}  # host -> {"total": n, "per_second": {epoch_second: n}}


def record(host: str) -> None:
    now = int(time.time())
    with _lock:
        entry = _counts.setdefault(host, {"total": 0, "per_second": {}})
        entry["total"] += 1
        entry["per_second"][now] = entry["per_second"].get(now, 0) + 1


def product_body(product_id: str) -> bytes:
    # Deterministic: the same id always gives the same bytes, so tests can compare exactly.
    return (
        "<html><head><title>Product {0}</title></head>"
        "<body><h1>Product {0}</h1><p>price: {1}.99 USD</p></body></html>\n"
    ).format(product_id, (sum(map(ord, product_id)) % 90) + 10).encode()


RETAILERS = {
    "northstar.pricelens.test": "Northstar",
    "harbor.pricelens.test": "Harbor Market",
    "copperlane.pricelens.test": "Copper Lane",
}


def offer(sku: str, host: str) -> dict:
    retailer = RETAILERS.get(host, "Sample retailer")
    # Stable per SKU and retailer, but different across stores, so the local demo
    # can show meaningful comparisons without depending on a live shopping site.
    store_offset = list(RETAILERS).index(host) * 7 if host in RETAILERS else 0
    price = 18.0 + ((sum(map(ord, sku)) + store_offset) % 4200) / 100
    return {"sku": sku, "retailer": retailer, "price": round(price, 2),
            "currency": "USD", "availability": "in_stock" if (sum(map(ord, sku)) + store_offset) % 4 else "low_stock",
            "checked_at_unix_ms": int(time.time() * 1000)}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # keep-alive, so Phase 5 can show connection reuse
    server_version = "testbed-target/1"

    def log_message(self, fmt, *args):  # no per-request logging (keeps logs free of URLs)
        pass

    def _send(self, status, body, ctype="text/html; charset=utf-8", head=False):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def _handle(self, head=False):
        path, _, query = self.path.partition("?")
        host = (self.headers.get("Host") or "unknown").split(":")[0].lower()
        params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)

        if path == "/health":
            return self._send(200, b'{"status":"ok"}', "application/json", head)
        if path == "/stats":
            with _lock:
                snap = {h: {"total": e["total"], "per_second": dict(e["per_second"])} for h, e in _counts.items()}
            return self._send(200, json.dumps(snap).encode(), "application/json", head)

        if path.startswith("/api/offers/") and len(path) > len("/api/offers/"):
            sku = unquote(path[len("/api/offers/"):])
            record(host)
            body = json.dumps(offer(sku, host)).encode()
            return self._send(200, body, "application/json", head)

        record(host)
        delay = DEFAULT_DELAY_MS
        if "delay_ms" in params and params["delay_ms"].isdigit():
            delay = int(params["delay_ms"])
        if delay > 0:
            time.sleep(delay / 1000.0)

        if path.startswith("/products/") and len(path) > len("/products/"):
            return self._send(200, product_body(path[len("/products/"):]), head=head)
        if path == "/":
            return self._send(200, b"testbed target\n", "text/plain", head)
        return self._send(404, b"not found\n", "text/plain", head)

    def do_GET(self):
        self._handle()

    def do_HEAD(self):
        self._handle(head=True)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        data = self.rfile.read(n) if n else b""
        record((self.headers.get("Host") or "unknown").split(":")[0].lower())
        self._send(200, ("received %d bytes\n" % len(data)).encode(), "text/plain")


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 2048


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "80"))
    Server((os.environ.get("BIND", "0.0.0.0"), port), Handler).serve_forever()
