#!/usr/bin/env python3
"""End-to-end test suite for the Intelligent Proxy Gateway (stdlib only).

    python3 scripts/e2e_test.py --build build [--only substring] [--docker]

Every test starts (and tears down) its own processes on fixed local ports 18000-19999.
"""
import argparse, csv, http.server, json, os, shutil, signal, socket, ssl, subprocess, sys
import tempfile, threading, time, traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
TMP = tempfile.mkdtemp(prefix="gw_e2e_")
GW, TARGET = 18000, 19000
PX = [18101, 18102, 18103]
PROCS = []


# ------------------------------------------------------------------ process helpers
def wait_port(port, timeout=5.0, hosts=("127.0.0.1", "127.0.0.2", "::1")):
    """Waits until `port` accepts connections on ANY of `hosts` (a server bound to ::1 only is not reachable over IPv4)."""
    end = time.time() + timeout
    while time.time() < end:
        for h in hosts:
            try:
                socket.create_connection((h, port), timeout=0.3).close()
                return True
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError(f"port {port} did not open")


def start(args, port=None, name=None):
    log = open(os.path.join(TMP, (name or os.path.basename(args[0])) + f".{len(PROCS)}.log"), "w")
    p = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
    PROCS.append(p)
    if port:
        try:
            wait_port(port)
        except RuntimeError:
            log.flush()
            raise RuntimeError(open(log.name).read() or "process failed to start")
    return p


def stop(p, sig=signal.SIGTERM, timeout=5):
    if p.poll() is None:
        p.send_signal(sig)
        try:
            p.wait(timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    return p.returncode


def cleanup():
    for p in PROCS:
        if p.poll() is None:
            p.kill()
            p.wait()
    PROCS.clear()


def merge(a, b):
    for k, v in b.items():
        if isinstance(v, dict) and isinstance(a.get(k), dict):
            merge(a[k], v)
        else:
            a[k] = v
    return a


def gw_cfg(proxies=None, **kw):
    cfg = {
        "listen_host": "127.0.0.1", "listen_port": GW, "policy": "round_robin",
        "workers": {"min": 2, "max": 32, "initial": 16, "alpha": 2, "beta": 0.5},
        "timeouts": {"connect_ms": 1000, "response_ms": 5000, "probe_interval_ms": 60000, "queue_ms": 30000},
        "circuit": {"failure_threshold": 5, "cooldown_sec": 2},
        "default_rate_limit": {"rate": 100000, "burst": 100000},
        "proxies": [({"host": "127.0.0.1", "port": p} if isinstance(p, int) else
                     dict({"host": "127.0.0.1", "port": p[0], "protocol": p[1]},
                          **({"username": p[2], "password": p[3]} if len(p) > 2 else {}))) for p in (proxies or PX)],
        "probe_target_url": f"http://127.0.0.1:{TARGET}/health",
        "output": {"csv_path": "", "json_path": ""},
    }
    return merge(cfg, kw)


def start_gateway(cfg, extra=()):
    path = os.path.join(TMP, f"cfg{len(PROCS)}.json")
    json.dump(cfg, open(path, "w"))
    return start([f"{BUILD}/gateway", "--config", path, *extra], cfg["listen_port"], "gateway")


def start_bed(proxies=((PX[0], 0, 0.0), (PX[1], 0, 0.0), (PX[2], 0, 0.0))):
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET)
    out = []
    for port, delay, fail in proxies:
        out.append(start([f"{BUILD}/proxy_node", "--port", str(port), "--delay-ms", str(delay),
                          "--fail-rate", str(fail)], port))
    return out


# --------------------------------------------------------------------- HTTP helpers
class Conn:
    def __init__(self, port=GW, timeout=10):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buf = b""

    def send(self, data):
        self.s.sendall(data if isinstance(data, bytes) else data.encode())

    def _fill(self):
        d = self.s.recv(65536)
        if not d:
            raise EOFError("connection closed")
        self.buf += d

    def response(self, head=False):
        while b"\r\n\r\n" not in self.buf:
            self._fill()
        hdr, _, rest = self.buf.partition(b"\r\n\r\n")
        lines = hdr.decode("latin1").split("\r\n")
        status = int(lines[0].split()[1])
        h = {}
        for ln in lines[1:]:
            k, _, v = ln.partition(":")
            h[k.strip().lower()] = v.strip()
        self.buf = rest
        if head or status in (204, 304):
            return status, h, b""
        if h.get("transfer-encoding", "").lower() == "chunked":
            body = b""
            while True:
                while b"\r\n" not in self.buf:
                    self._fill()
                line, _, self.buf = self.buf.partition(b"\r\n")
                n = int(line.split(b";")[0], 16)
                while len(self.buf) < n + 2:
                    self._fill()
                body += self.buf[:n]
                self.buf = self.buf[n + 2:]
                if n == 0:
                    break
            return status, h, body
        if "content-length" in h:
            n = int(h["content-length"])
            while len(self.buf) < n:
                self._fill()
            body, self.buf = self.buf[:n], self.buf[n:]
            return status, h, body
        body = self.buf
        try:
            while True:
                d = self.s.recv(65536)
                if not d:
                    break
                body += d
        except OSError:
            pass
        self.buf = b""
        return status, h, body

    def is_closed(self, wait=0.3):
        self.s.settimeout(wait)
        try:
            return self.s.recv(1) == b""
        except socket.timeout:
            return False
        except OSError:
            return True

    def close(self):
        self.s.close()


def get(url, port=GW, method="GET", body=b"", headers="", close=True):
    c = Conn(port)
    cl = f"Content-Length: {len(body)}\r\n" if body or method in ("POST", "PUT") else ""
    c.send(f"{method} {url} HTTP/1.1\r\nHost: x\r\n{headers}{cl}" + ("Connection: close\r\n" if close else "") + "\r\n")
    if body:
        c.send(body)
    try:
        return c.response(head=(method == "HEAD"))
    finally:
        c.close()


def T(path):
    return f"http://127.0.0.1:{TARGET}{path}"


def metrics(port=GW):
    status, _, body = get("/metrics", port)
    assert status == 200
    out = {}
    for ln in body.decode().splitlines():
        if ln and not ln.startswith("#") and "{" not in ln:
            k, v = ln.split()
            out[k] = float(v)
    return out


def proxies_state(port=GW):
    return json.loads(get("/proxies", port)[2])


def curl(args, timeout=30):
    # the sandbox/CI may export http(s)_proxy variables: scrub them so only the explicit -x proxy is used
    env = {k: v for k, v in os.environ.items() if k.lower() not in ("http_proxy", "https_proxy", "all_proxy", "no_proxy")}
    r = subprocess.run(["curl", "-s", "-m", str(timeout), *args], capture_output=True, timeout=timeout + 5, env=env)
    return r.returncode, r.stdout, r.stderr


def check(cond, msg=""):
    if not cond:
        raise AssertionError(msg or "check failed")


# ------------------------------------------------------------------------------ tests
def test_basic_methods():
    start_bed(); start_gateway(gw_cfg())
    s, h, b = get(T("/products/1"))
    check(s == 200 and json.loads(b)["path"] == "/products/1", (s, b))
    s, h, b = get(T("/x"), method="HEAD")
    check(s == 200 and b == b"" and int(h["content-length"]) > 0, "HEAD")
    s, h, b = get(T("/x"), method="DELETE"); check(s == 200 and json.loads(b)["method"] == "DELETE")
    s, h, b = get(T("/x"), method="OPTIONS"); check(s == 200)
    rc, out, _ = curl(["-x", f"http://127.0.0.1:{GW}", T("/via-curl")])
    check(rc == 0 and json.loads(out)["path"] == "/via-curl", "curl GET")


def test_request_bodies():
    start_bed(); start_gateway(gw_cfg())
    for size in (1, 1000, 100_000, 3_000_000):
        data = os.urandom(size)
        path = os.path.join(TMP, "body.bin"); open(path, "wb").write(data)
        rc, out, err = curl(["-x", f"http://127.0.0.1:{GW}", "--data-binary", f"@{path}", T("/upload")])
        check(rc == 0 and json.loads(out)["body_bytes"] == size and json.loads(out)["method"] == "POST", (size, rc, out, err))
    rc, out, _ = curl(["-x", f"http://127.0.0.1:{GW}", "-X", "PUT", "-d", "abcdef", T("/put")])
    check(json.loads(out)["method"] == "PUT" and json.loads(out)["body_bytes"] == 6, out)
    # chunked request body
    rc, out, _ = curl(["-x", f"http://127.0.0.1:{GW}", "-H", "Transfer-Encoding: chunked", "-d", "hello chunked", T("/ch")])
    check(rc == 0 and json.loads(out)["body_bytes"] == 13, (rc, out))
    # Expect: 100-continue with a body larger than curl's threshold
    data = os.urandom(2_000_000); path = os.path.join(TMP, "big.bin"); open(path, "wb").write(data)
    rc, out, _ = curl(["-x", f"http://127.0.0.1:{GW}", "-H", "Expect: 100-continue", "--data-binary", f"@{path}", T("/exp")])
    check(rc == 0 and json.loads(out)["body_bytes"] == 2_000_000, (rc, out))
    # over the body limit -> 413 without reading the body
    c = Conn(); c.send(f"POST {T('/x')} HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999\r\n\r\n")
    check(c.response()[0] == 413, "oversized body rejected up front"); c.close()


def test_keepalive_pipelining_partial():
    start_bed(); start_gateway(gw_cfg())
    # many requests on ONE client connection
    c = Conn()
    for i in range(25):
        c.send(f"GET {T('/k/' + str(i))} HTTP/1.1\r\nHost: x\r\n\r\n")
        s, h, b = c.response()
        check(s == 200 and json.loads(b)["path"] == f"/k/{i}" and h.get("connection") != "close", (i, s, h))
    check(metrics()["client_connections"] >= 1)
    # POST then GET on the same connection
    c.send(f"POST {T('/p')} HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nabcd")
    check(json.loads(c.response()[2])["body_bytes"] == 4)
    c.send(f"GET {T('/after')} HTTP/1.1\r\nHost: x\r\n\r\n")
    check(json.loads(c.response()[2])["path"] == "/after")
    # Connection: close is honoured
    c.send(f"GET {T('/bye')} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    check(c.response()[0] == 200 and c.is_closed(1.0), "close after Connection: close")
    # pipelining: 3 requests in a single packet, answered in order
    c = Conn()
    c.send("".join(f"GET {T('/pl/' + str(i))} HTTP/1.1\r\nHost: x\r\n\r\n" for i in range(3)))
    for i in range(3):
        check(json.loads(c.response()[2])["path"] == f"/pl/{i}", f"pipelined {i}")
    c.close()
    # partial requests: header and body dribbled a few bytes at a time
    msg = f"POST {T('/slowly')} HTTP/1.1\r\nHost: x\r\nContent-Length: 12\r\n\r\nhello world!".encode()
    c = Conn()
    for i in range(0, len(msg), 5):
        c.send(msg[i:i + 5]); time.sleep(0.02)
    s, _, b = c.response()
    check(s == 200 and json.loads(b)["body_bytes"] == 12, (s, b))
    # request split exactly at the header/body boundary with a pause
    c = Conn()
    c.send(f"POST {T('/split')} HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n"); time.sleep(0.3); c.send("wxyz")
    check(json.loads(c.response()[2])["body_bytes"] == 4)
    c.close()


def test_response_variants():
    start_bed(); start_gateway(gw_cfg())
    c = Conn()
    c.send(f"GET {T('/chunked')} HTTP/1.1\r\nHost: x\r\n\r\n")
    s, h, b = c.response(); check(s == 200 and b == b"hello world!", (s, b))
    c.send(f"GET {T('/health')} HTTP/1.1\r\nHost: x\r\n\r\n")   # connection stays usable after chunked reply
    check(c.response()[0] == 200, "keep-alive after chunked")
    c.close()
    s, h, b = get(T("/close"), close=False); check(s == 200 and b == b"unframed-body", (s, b))
    s, h, b = get(T("/big?bytes=5000000")); check(s == 200 and len(b) == 5_000_000, len(b))
    s, h, b = get(T("/status/404")); check(s == 404, s)
    s, h, b = get(T("/status/500")); check(s == 500 and b.startswith(b"status 500"), (s, b))  # origin 5xx passed through
    check(proxies_state() is not None)


def test_tls_connect_tunnel():
    cert, key = os.path.join(TMP, "c.pem"), os.path.join(TMP, "k.pem")
    r = subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                        "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=localhost"], capture_output=True)
    if r.returncode != 0:
        raise Skip("openssl unavailable")

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        def do_GET(self):
            b = b"secure-ok " + self.path.encode()
            self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
        def log_message(self, *a): pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 19443), H)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        start_bed(); start_gateway(gw_cfg())
        rc, out, err = curl(["-k", "-x", f"http://127.0.0.1:{GW}", "https://127.0.0.1:19443/hello"])
        check(rc == 0 and out == b"secure-ok /hello", (rc, out, err))
        # two requests over one tunnel
        rc, out, _ = curl(["-k", "-x", f"http://127.0.0.1:{GW}", "https://127.0.0.1:19443/a", "https://127.0.0.1:19443/b"])
        check(out == b"secure-ok /asecure-ok /b", out)
        m = metrics(); check(m["tunnels_total"] >= 2, m)
        # bytes tunnelled through the gateway are really TLS (client verifies with our cert -> hostname OK)
        rc, out, _ = curl(["--cacert", cert, "-x", f"http://127.0.0.1:{GW}", "https://localhost:19443/verified"])
        check(rc == 0 and out == b"secure-ok /verified", (rc, out))
        # failures: closed port -> 502; invalid target -> 400
        c = Conn(); c.send("CONNECT 127.0.0.1:1 HTTP/1.1\r\nHost: x\r\n\r\n"); check(c.response()[0] == 502)
        c = Conn(); c.send("CONNECT nonsense HTTP/1.1\r\nHost: x\r\n\r\n"); check(c.response()[0] == 400)
        # plain-text absolute https:// is rejected (must use CONNECT)
        check(get("https://127.0.0.1:19443/x")[0] == 400)
        # idle tunnel is cleaned up on gateway shutdown
        c = Conn(); c.send("CONNECT 127.0.0.1:19443 HTTP/1.1\r\nHost: x\r\n\r\n"); check(c.response(head=True)[0] == 200)
        check(metrics()["active_tunnels"] >= 1)
        c.close()   # the python TLS server handshakes inside accept(): release it before shutdown
        time.sleep(0.3)
        check(metrics()["active_tunnels"] == 0, "tunnel counter drops when the client disconnects")
    finally:
        srv.shutdown()


def test_socks5_and_proxy_node_connect():
    start_bed()
    rc, out, err = curl(["--socks5-hostname", f"127.0.0.1:{PX[0]}", T("/via-socks")])
    check(rc == 0 and json.loads(out)["path"] == "/via-socks", (rc, out, err))
    rc, out, err = curl(["--socks5", f"127.0.0.1:{PX[1]}", T("/via-socks-ip")])
    check(rc == 0 and json.loads(out)["path"] == "/via-socks-ip", (rc, out, err))
    # raw protocol: greeting, CONNECT by domain name, then HTTP over the relay
    s = socket.create_connection(("127.0.0.1", PX[0]), timeout=5)
    s.sendall(b"\x05\x01\x00"); check(s.recv(2) == b"\x05\x00", "greeting")
    host = b"localhost"
    s.sendall(b"\x05\x01\x00\x03" + bytes([len(host)]) + host + TARGET.to_bytes(2, "big"))
    check(s.recv(10)[:2] == b"\x05\x00", "connect reply")
    s.sendall(b"GET /raw HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    data = b""
    while True:
        d = s.recv(4096)
        if not d: break
        data += d
    check(b'"path":"/raw"' in data, data)
    # unsupported command (BIND) and refused target
    s = socket.create_connection(("127.0.0.1", PX[0]), timeout=5); s.sendall(b"\x05\x01\x00"); s.recv(2)
    s.sendall(b"\x05\x02\x00\x01\x7f\x00\x00\x01\x00\x50"); check(s.recv(10)[1] == 0x07, "BIND rejected")
    s = socket.create_connection(("127.0.0.1", PX[0]), timeout=5); s.sendall(b"\x05\x01\x00"); s.recv(2)
    s.sendall(b"\x05\x01\x00\x01\x7f\x00\x00\x01\x00\x01"); check(s.recv(10)[1] == 0x05, "connection refused code")
    # no acceptable auth method
    s = socket.create_connection(("127.0.0.1", PX[0]), timeout=5); s.sendall(b"\x05\x01\x02"); check(s.recv(2) == b"\x05\xff")
    # HTTP CONNECT straight against the proxy node
    c = Conn(PX[2]); c.send(f"CONNECT 127.0.0.1:{TARGET} HTTP/1.1\r\nHost: x\r\n\r\n"); check(c.response(head=True)[0] == 200)
    c.send("GET /tunnel HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"); time.sleep(0.3)
    check(b'"path":"/tunnel"' in c.s.recv(4096), "data through CONNECT")


def test_input_validation():
    start_bed(); start_gateway(gw_cfg(limits={"max_header_bytes": 4096}))
    def raw(data, wait=True):
        c = Conn(); c.send(data)
        try: return c.response()
        except EOFError: return (0, {}, b"")
        finally: c.close()
    check(raw("GARBAGE\r\n\r\n")[0] == 400)
    check(raw("GET http://h/ HTTP/9.9\r\n\r\n")[0] == 400)
    check(raw("GET /metrics HTTP/1.1\r\nBad Header\r\n\r\n")[0] == 400)
    check(raw(f"GET {T('/')} HTTP/1.1\r\nX: " + "a" * 8000 + "\r\n\r\n")[0] == 431)
    check(raw(f"POST {T('/')} HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n")[0] == 400)
    check(raw(f"POST {T('/')} HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n")[0] == 400)
    check(raw(f"TRACE {T('/')} HTTP/1.1\r\n\r\n")[0] == 501)
    check(raw("GET /nope HTTP/1.1\r\n\r\n")[0] == 404)
    check(raw("POST /metrics HTTP/1.1\r\nContent-Length: 0\r\n\r\n")[0] == 405)
    check(raw("GET ftp://h/x HTTP/1.1\r\n\r\n")[0] == 400)
    check(raw("GET http://h:99999/x HTTP/1.1\r\n\r\n")[0] == 400)
    s, h, b = raw("GET /health HTTP/1.1\r\n\r\n"); check(s == 200 and json.loads(b)["status"] == "healthy")
    # gateway still healthy after all that
    check(get(T("/ok"))[0] == 200)


def test_timeouts_idle_and_slowloris():
    start_bed()
    start_gateway(gw_cfg(timeouts={"client_idle_ms": 600, "client_header_ms": 600}))
    c = Conn(); c.send(f"GET {T('/a')} HTTP/1.1\r\nHost: x\r\n\r\n"); check(c.response()[0] == 200)
    time.sleep(2.2); check(c.is_closed(0.5), "idle keep-alive connection must be closed by the server")
    c = Conn(); c.send("GET http://127.0.0.1/ HTTP/1.1\r\nHost: x\r\n")   # headers never finish (slow-loris)
    time.sleep(2.2); check(c.is_closed(0.5), "incomplete request must time out")
    c = Conn(); c.send(f"GET {T('/b')} HTTP/1.1\r\nHost: x\r\n\r\n"); check(c.response()[0] == 200)  # server unaffected
    c.close()
    check(metrics()["client_connections"] <= 1)


def test_queue_timeout_and_overflow():
    start_bed()
    # 1 req/s, burst 1, queue deadline 1.5s: request #1 immediate, #2 at ~1s, the rest must expire with 504
    start_gateway(gw_cfg(default_rate_limit={"rate": 1.0, "burst": 1.0}, timeouts={"queue_ms": 1500}, max_queue_size=50))
    results = []
    def one(i):
        t0 = time.time(); r = get(T(f"/q/{i}"))
        results.append((i, r[0], time.time() - t0))
    ts = [threading.Thread(target=one, args=(i,)) for i in range(6)]
    [t.start() for t in ts]; [t.join(15) for t in ts]
    codes = sorted(r[1] for r in results)
    check(len(results) == 6, "all clients must get an answer (no hangs)")
    check(codes.count(200) in (2, 3) and codes.count(504) >= 3, codes)
    check(all(r[2] < 3.0 for r in results), results)
    m = metrics(); check(m["queue_timeouts"] >= 3, m)
    check(metrics()["queue_length"] == 0)
    # overflow: tiny queue, slow refill -> immediate 503
    cleanup(); start_bed()
    start_gateway(gw_cfg(default_rate_limit={"rate": 0.5, "burst": 1.0}, timeouts={"queue_ms": 4000}, max_queue_size=3))
    res = []
    ts = [threading.Thread(target=lambda i=i: res.append(get(T(f"/o/{i}"))[0])) for i in range(12)]
    [t.start() for t in ts]; [t.join(20) for t in ts]
    check(res.count(503) >= 6 and len(res) == 12, sorted(res))


def test_log_escaping():
    start_bed()
    csv_p, json_p = os.path.join(TMP, "r.csv"), os.path.join(TMP, "r.jsonl")
    gw = start_gateway(gw_cfg(output={"csv_path": csv_p, "json_path": json_p}))
    nasty = [b'/a"b,c', b'/q%22uote\\back', b"/utf-\xc3\xa9", b"/bad-\xff-byte", b"/tab\x09ctl\x01"]
    for p in nasty:
        c = Conn()
        c.send(b"GET http://127.0.0.1:%d%s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" % (TARGET, p))
        try: c.response()
        except EOFError: pass
        c.close()
    for i in range(3): get(T(f"/plain/{i}"))
    stop(gw)  # flush
    rows = list(csv.reader(open(csv_p, newline="", encoding="utf-8", errors="strict")))
    check(rows[0][0] == "timestamp_ms" and all(len(r) == 6 for r in rows), [r for r in rows if len(r) != 6])
    urls = [r[1] for r in rows[1:]]
    check(any(u.endswith('/a"b,c') for u in urls), urls)
    lines = open(json_p, encoding="utf-8", errors="strict").read().splitlines()
    docs = [json.loads(l) for l in lines]   # every line must be valid JSON
    check(len(docs) == len(rows) - 1 and any(d["url"].endswith('/a"b,c') for d in docs), len(docs))
    check(any("\ufffd" in d["url"] for d in docs), "invalid UTF-8 replaced with U+FFFD")
    check(len(rows) - 1 >= 5, len(rows))   # one record per request, none lost/merged (control chars rejected by parser => 400)


def test_config_validation():
    def run(text, name="c.json"):
        p = os.path.join(TMP, name); open(p, "w").write(text)
        r = subprocess.run([f"{BUILD}/gateway", "--config", p, "--check-config"], capture_output=True, text=True, timeout=10)
        return r.returncode, r.stdout + r.stderr
    for shipped in ("gateway.json", "gateway.docker.json", "loadtest.json"):
        os.environ["GATEWAY_ADMIN_TOKEN"] = "test-token-1234567890"      # the Docker config reads its admin token from the environment
        rc, out = run(open(os.path.join(ROOT, "config", shipped)).read(), shipped)
        check(rc == 0 and "Configuration OK" in out, (shipped, out))
    del os.environ["GATEWAY_ADMIN_TOKEN"]
    rc, out = run(open(os.path.join(ROOT, "config", "gateway.docker.json")).read(), "nodefault.json")
    check(rc == 1 and "GATEWAY_ADMIN_TOKEN" in out, "the Docker config must refuse to start without an admin token")
    ugly = '{"proxies"\n:\n[\t{"port"   :8101,\n"host":"127.0.0.1"}],"policy":\r\n"weighted","workers":{"initial":4,"max":8,"min":1}}'
    rc, out = run(ugly); check(rc == 0, out)
    rc, out = run('{\n "proxies": [{"host":"h","port":1}],\n "policy": ?\n}'); check(rc == 1 and "line 3" in out, out)
    rc, out = run('{"proxies":[{"host":"h","port":1}],}'); check(rc == 1 and "parse error" in out, out)
    rc, out = run('{"proxies":[{"host":"h","port":"1"}]}'); check(rc == 1 and "proxies[0].port" in out, out)
    rc, out = run('{"proxies":[{"host":"h","port":1}],"workers":{"min":9,"max":3}}'); check(rc == 1 and "workers" in out, out)
    rc, out = run('{"proxies":[]}'); check(rc == 1, out)
    rc, out = run('{"proxies":[{"host":"h","port":1}], "default_rate_limit":{"rate":5}}'); check(rc == 1 and "burst" in out, out)
    rc, out = run('{"proxies":[{"host":"h","port":1}], "mystery": 1}'); check(rc == 0 and "unknown key 'mystery'" in out, out)
    r = subprocess.run([f"{BUILD}/gateway", "--config", "/nonexistent.json"], capture_output=True, text=True); check(r.returncode == 1)
    for bad in (["--workers", "abc"], ["--workers", "0"], ["--port", "70000"], ["--policy", "x"], ["--nope"]):
        r = subprocess.run([f"{BUILD}/gateway", *bad, "--check-config"], capture_output=True, text=True)
        check(r.returncode == 1, bad)
    # a second gateway on the same port must FAIL loudly (no silent SO_REUSEPORT sharing)
    start_bed(); start_gateway(gw_cfg())
    p = start([f"{BUILD}/gateway", "--config", os.path.join(ROOT, "config", "loadtest.json"), "--port", str(GW)])
    p.wait(5); check(p.returncode == 1, "second gateway on same port must exit with an error")


def test_failover_retries_and_all_down():
    px = start_bed(((PX[0], 0, 0.0), (PX[1], 20, 0.0), (PX[2], 0, 1.0)))
    start_gateway(gw_cfg())
    codes = [get(T(f"/f/{i}"))[0] for i in range(60)]
    check(codes.count(200) == 60, ("retries must mask the 100% failing proxy", codes))
    st = {p["endpoint"]: p for p in proxies_state()}
    check(st[f"127.0.0.1:{PX[2]}"]["circuit_state"] == "OPEN", st)
    stop(px[0], signal.SIGKILL)       # kill another proxy mid-run
    check(all(get(T(f"/g/{i}"))[0] == 200 for i in range(30)), "failover after kill")
    stop(px[1], signal.SIGKILL)
    codes = [get(T(f"/h/{i}"))[0] for i in range(10)]
    check(all(c in (502, 503) for c in codes) and codes.count(503) >= 5, codes)


def test_circuit_half_open_to_closed_live_traffic():
    # probes effectively disabled (60 s), so recovery can only come from live traffic
    px = start_bed(((PX[0], 0, 0.0), (PX[1], 0, 1.0)))
    start_gateway(gw_cfg(proxies=[PX[0], PX[1]], circuit={"failure_threshold": 3, "cooldown_sec": 2}))
    for i in range(20): get(T(f"/c/{i}"))
    st = {p["endpoint"]: p for p in proxies_state()}[f"127.0.0.1:{PX[1]}"]
    check(st["circuit_state"] == "OPEN", st)
    # OPEN proxies receive no traffic during the cooldown
    before = st["failure_count"]
    for i in range(10): get(T(f"/d/{i}"))
    check({p["endpoint"]: p for p in proxies_state()}[f"127.0.0.1:{PX[1]}"]["failure_count"] == before, "no requests while OPEN")
    # replace the broken proxy with a healthy one on the same port
    stop(px[1], signal.SIGKILL); time.sleep(0.2)
    start([f"{BUILD}/proxy_node", "--port", str(PX[1]), "--fail-rate", "0"], PX[1])
    time.sleep(2.3)  # cooldown
    states = []
    for i in range(10):
        check(get(T(f"/r/{i}"))[0] == 200)
        states.append({p["endpoint"]: p for p in proxies_state()}[f"127.0.0.1:{PX[1]}"]["circuit_state"])
    check(states[-1] == "CLOSED", states)
    final = {p["endpoint"]: p for p in proxies_state()}[f"127.0.0.1:{PX[1]}"]
    check(final["success_count"] >= 1 and final["consecutive_failures"] == 0, final)


def test_post_not_retried_get_is_retried():
    start_bed(((PX[0], 0, 1.0), (PX[1], 0, 1.0)))
    start_gateway(gw_cfg(proxies=[PX[0], PX[1]], circuit={"failure_threshold": 1000}))
    fails = lambda: sum(p["failure_count"] for p in proxies_state())
    base = fails()
    check(get(T("/x"))[0] in (502, 503)); g = fails() - base
    base = fails()
    check(get(T("/x"), method="POST", body=b"data")[0] in (502, 503)); p = fails() - base
    check(g == 2 and p == 1, f"GET attempts={g} (expect 2: both proxies), POST attempts={p} (expect 1: not retried)")


def test_idle_cpu_and_clean_shutdown():
    start_bed()
    gw = start_gateway(gw_cfg(workers={"min": 4, "max": 64, "initial": 32}))
    for i in range(20): get(T(f"/w/{i}"))
    time.sleep(1.0)
    tck = os.sysconf("SC_CLK_TCK")
    def cpu():
        f = open(f"/proc/{gw.pid}/stat").read().rsplit(")", 1)[1].split()
        return (int(f[11]) + int(f[12])) / tck
    a = cpu(); time.sleep(4.0); used = cpu() - a
    check(used < 0.15, f"idle gateway with 64 worker threads used {used:.3f}s CPU in 4s (expected ~0)")
    # clean shutdown with an open keep-alive connection and a live tunnel
    c = Conn(); c.send(f"GET {T('/z')} HTTP/1.1\r\nHost: x\r\n\r\n"); c.response()
    t = Conn(); t.send(f"CONNECT 127.0.0.1:{TARGET} HTTP/1.1\r\nHost: x\r\n\r\n"); check(t.response(head=True)[0] == 200)
    t0 = time.time(); rc = stop(gw, timeout=8)
    check(rc == 0 and time.time() - t0 < 4, (rc, time.time() - t0))


def test_aimd_and_concurrency_bound():
    # slow proxies + limit of 4 concurrent executions: upstream concurrency must never exceed the limit
    start_bed(((PX[0], 200, 0.0),))
    start_gateway(gw_cfg(proxies=[PX[0]], workers={"min": 4, "max": 4, "initial": 4}))
    peak = [0]; stopf = [False]
    def sample():
        while not stopf[0]:
            try: peak[0] = max(peak[0], int(metrics()["active_workers"]))
            except Exception: pass
            time.sleep(0.02)
    th = threading.Thread(target=sample); th.start()
    ts = [threading.Thread(target=lambda i=i: get(T(f"/s/{i}"))) for i in range(24)]
    t0 = time.time(); [t.start() for t in ts]; [t.join(30) for t in ts]
    el = time.time() - t0; stopf[0] = True; th.join()
    check(peak[0] <= 4, f"active workers peaked at {peak[0]} > limit 4")
    check(el >= 1.0, f"24 x 200ms requests at concurrency 4 should take >= 1.2s, took {el:.2f}")


def thread_count(pid):
    return len(os.listdir(f"/proc/{pid}/task"))


def test_inflight_requests_do_not_hold_threads():
    # One slow proxy (1 s per request). 400 simultaneous requests must all be IN FLIGHT at once while the
    # gateway keeps a handful of threads. A thread-per-request design needs 400 threads (or serialises them).
    start_bed(((PX[0], 1000, 0.0),))
    gw = start_gateway(gw_cfg(proxies=[PX[0]], workers={"min": 4, "max": 2000, "initial": 1000, "io_threads": 2},
                              timeouts={"response_ms": 8000}))
    base_threads = thread_count(gw.pid)
    peak_threads, peak_inflight, stopf = [0], [0], [False]
    def sample():
        while not stopf[0]:
            try:
                peak_threads[0] = max(peak_threads[0], thread_count(gw.pid))
                peak_inflight[0] = max(peak_inflight[0], int(metrics()["active_workers"]))
            except Exception: pass
            time.sleep(0.05)
    sm = threading.Thread(target=sample); sm.start()
    codes = []
    ts = [threading.Thread(target=lambda i=i: codes.append(get(T(f"/m/{i}"))[0])) for i in range(400)]
    t0 = time.time(); [t.start() for t in ts]; [t.join(40) for t in ts]
    el = time.time() - t0; stopf[0] = True; sm.join()
    check(codes.count(200) == 400, (len(codes), set(codes)))
    check(peak_inflight[0] >= 300, f"only {peak_inflight[0]} requests were in flight simultaneously")
    check(peak_threads[0] <= base_threads + 3, f"gateway threads grew from {base_threads} to {peak_threads[0]}")
    check(peak_threads[0] <= 12, f"{peak_threads[0]} threads for 400 in-flight requests")
    check(el < 5.0, f"400 x 1s requests took {el:.1f}s (should overlap: ~1-2s)")


def test_slow_client_and_slow_upstream_block_nothing():
    start_bed(((PX[0], 0, 0.0), (PX[1], 0, 0.0)))
    gw = start_gateway(gw_cfg(proxies=[PX[0], PX[1]], workers={"min": 2, "max": 256, "initial": 128, "io_threads": 1}))
    # 1) a client that asks for 40 MB and never reads: its write must park in epoll, not block an I/O thread
    lazy = []
    for _ in range(6):
        c = Conn(); c.send(f"GET {T('/big?bytes=40000000')} HTTP/1.1\r\nHost: x\r\n\r\n"); lazy.append(c)
    time.sleep(1.0)
    t0 = time.time()
    for i in range(40):
        check(get(T(f"/fast/{i}"))[0] == 200, "healthy requests must still be served")
    check(time.time() - t0 < 3.0, f"40 requests took {time.time() - t0:.1f}s while slow clients were stalled")
    check(thread_count(gw.pid) <= 12, thread_count(gw.pid))
    # the stalled replies are bounded by the 10 s client-write timeout, then released
    for c in lazy: c.close()
    deadline = time.time() + 20      # generous: sanitizer builds are 5-15x slower; a real leak never drains
    while metrics()["active_workers"] != 0 and time.time() < deadline: time.sleep(0.1)
    check(metrics()["active_workers"] == 0, "no leaked in-flight slots")
    # (a very slow build may have timed the 40 MB transfers out and opened the circuit: it must close again by itself)
    deadline = time.time() + 20
    while get(T("/after"))[0] != 200 and time.time() < deadline: time.sleep(0.3)
    check(get(T("/after"))[0] == 200 and metrics()["active_workers"] == 0, "gateway serves normally after the stalled clients")
    # 2) a hung origin (30 s) must not stop fast traffic either
    hung = [threading.Thread(target=lambda: get(T("/slow?ms=30000"))) for _ in range(20)]
    [h.start() for h in hung]; time.sleep(0.5)
    t0 = time.time()
    check(all(get(T(f"/quick/{i}"))[0] == 200 for i in range(30)), "fast requests next to hung ones")
    check(time.time() - t0 < 2.0)
    check(thread_count(gw.pid) <= 12, thread_count(gw.pid))
    check(metrics()["active_workers"] >= 20, "hung requests are in flight, not queued behind threads")


def test_async_timeouts_and_stale_connections():
    px = start_bed(((PX[0], 0, 0.0),))
    start_gateway(gw_cfg(proxies=[PX[0]], max_retries=0, timeouts={"response_ms": 1000, "connect_ms": 500}))
    # response timeout enforced by timers (no blocking read): /slow?ms=3000 -> 504 after ~1 s
    t0 = time.time(); s, _, _ = get(T("/slow?ms=3000")); el = time.time() - t0
    check(s == 504 and 0.8 < el < 2.0, (s, el))
    check(metrics()["timeouts"] >= 1)
    # connect failure -> fast 502 (nothing listening where the proxy used to be)
    check(get(T("/ok"))[0] in (200, 502, 504))
    # pooled keep-alive connection goes stale (proxy restarted): the next request must still succeed
    cleanup(); start_bed(((PX[0], 0, 0.0),))
    gw = start_gateway(gw_cfg(proxies=[PX[0]], max_retries=0))
    for i in range(5): check(get(T(f"/warm/{i}"))[0] == 200)
    pn = [p for p in PROCS if p.args[0].endswith("proxy_node")][0]
    stop(pn, signal.SIGKILL); time.sleep(0.2)
    start([f"{BUILD}/proxy_node", "--port", str(PX[0])], PX[0])
    codes = [get(T(f"/after-restart/{i}"))[0] for i in range(5)]
    check(codes == [200] * 5, ("stale pooled connection must be replayed on a fresh one", codes))
    # proxy dies completely: clean 502/503, no hang, and recovery when it returns
    stop(pn, signal.SIGKILL)
    for p in list(PROCS):
        if p.args[0].endswith("proxy_node") and p.poll() is None: stop(p, signal.SIGKILL)
    t0 = time.time(); s = get(T("/dead"))[0]
    check(s in (502, 503) and time.time() - t0 < 3, (s, time.time() - t0))


def open_tunnel(target=None):
    c = Conn(timeout=20)
    c.send(f"CONNECT 127.0.0.1:{target or TARGET} HTTP/1.1\r\nHost: x\r\n\r\n")
    check(c.response(head=True)[0] == 200, "CONNECT must succeed")
    return c


def test_tunnels_use_no_threads():
    start_bed(((PX[0], 0, 0.0),))
    gw = start_gateway(gw_cfg(proxies=[PX[0]], workers={"min": 4, "max": 2000, "initial": 1000, "io_threads": 2},
                              timeouts={"tunnel_idle_ms": 3000}))
    base = thread_count(gw.pid)
    # 300 live tunnels at once, each carrying a real request/response
    tunnels = [open_tunnel() for _ in range(300)]
    for i, c in enumerate(tunnels):
        c.send(f"GET /t/{i} HTTP/1.1\r\nHost: x\r\n\r\n")
    for i, c in enumerate(tunnels):
        st, _, body = c.response()
        check(st == 200 and f'"path":"/t/{i}"'.encode() in body, (i, st, body[:80]))
    m = metrics()
    check(m["active_tunnels"] == 300, m["active_tunnels"])
    check(thread_count(gw.pid) <= base + 1 and thread_count(gw.pid) <= 12,
          f"{thread_count(gw.pid)} threads (start {base}) for 300 live tunnels")
    # tunnels are independent: traffic on one while 299 idle ones sit open
    tunnels[7].send("GET /again HTTP/1.1\r\nHost: x\r\n\r\n")
    check(tunnels[7].response()[0] == 200)
    # idle tunnels are reaped by the engine's timer (3 s idle limit; generous so sanitizer builds cannot trip it while setting up), and the counters drop to zero
    deadline = time.time() + 12
    while metrics()["active_tunnels"] and time.time() < deadline: time.sleep(0.2)
    check(metrics()["active_tunnels"] == 0, metrics()["active_tunnels"])
    try:
        tunnels[0].s.settimeout(2); closed = tunnels[0].s.recv(10) == b""
    except (ConnectionResetError, BrokenPipeError):
        closed = True
    check(closed, "idle tunnel must be closed by the gateway")
    # normal client close also releases the tunnel
    c = open_tunnel(); check(metrics()["active_tunnels"] == 1); c.s.close(); time.sleep(0.4)
    check(metrics()["active_tunnels"] == 0)
    check(get(T("/after"))[0] == 200 and metrics()["active_workers"] == 0, "no leaked in-flight slots")


def test_tunnel_backpressure_and_halfclose():
    start_bed(((PX[0], 0, 0.0),))
    gw = start_gateway(gw_cfg(proxies=[PX[0]], workers={"min": 2, "max": 64, "initial": 16, "io_threads": 1}))
    # 1) a large body through the tunnel to a deliberately slow reader: back-pressure, no loss, no corruption
    c = open_tunnel()
    c.send("GET /big?bytes=30000000 HTTP/1.1\r\nHost: x\r\n\r\n")
    got = 0; first = True
    c.s.settimeout(20)
    while True:
        while b"\r\n\r\n" not in c.buf and first: c._fill()
        if first:
            hdr, _, rest = c.buf.partition(b"\r\n\r\n"); first = False
            clen = int([l for l in hdr.decode().split("\r\n") if l.lower().startswith("content-length")][0].split(":")[1])
            got = len(rest); c.buf = b""
        if got >= clen: break
        d = c.s.recv(65536)
        if not d: break
        got += len(d)
        if got < 2_000_000: time.sleep(0.002)   # slow start: the gateway must buffer, not drop
    check(got == clen == 30000000, (got, clen))
    check(thread_count(gw.pid) <= 8, thread_count(gw.pid))
    c.s.close()
    # 2) half-close: client sends its request and shuts its write side; the response must still arrive in full
    c = open_tunnel()
    c.send("GET /halfclose HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    c.s.shutdown(socket.SHUT_WR)
    data = b""
    c.s.settimeout(10)
    while True:
        d = c.s.recv(65536)
        if not d: break
        data += d
    check(data.startswith(b"HTTP/1.1 200") and b'"path":"/halfclose"' in data, data[:100])
    time.sleep(0.3)
    check(metrics()["active_tunnels"] == 0, "tunnel finishes after both directions are shut")
    # 3) upstream (origin) closes first: the client sees EOF and the tunnel is cleaned up
    c = open_tunnel()
    c.send("GET /close HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    c.s.settimeout(10); d = b""
    while True:
        x = c.s.recv(65536)
        if not x: break
        d += x
    check(d.startswith(b"HTTP/1.1 200"), d[:60])
    c.s.close(); time.sleep(0.3)
    check(metrics()["active_tunnels"] == 0)


def start_node(port, only="", delay=0, auth=""):
    args = [f"{BUILD}/proxy_node", "--port", str(port), "--delay-ms", str(delay)]
    if only: args += ["--only", only]
    if auth: args += ["--auth", auth]
    return start(args, port)


def test_upstream_socks5():
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET)
    n_socks = start_node(PX[0], "socks5")      # speaks ONLY SOCKS5: any HTTP request to it is dropped
    n_http = start_node(PX[1], "http")         # speaks ONLY HTTP
    gw = start_gateway(gw_cfg(proxies=[(PX[0], "socks5")]))
    st = proxies_state()[0]; check(st["protocol"] == "socks5", st)
    # plain requests, query string preserved, many over pooled SOCKS5 tunnels
    s, _, b = get(T("/products/1?x=1&y=2")); check(s == 200 and json.loads(b)["path"].startswith("/products/1"), (s, b))
    for i in range(60): check(get(T(f"/seq/{i}"))[0] == 200, i)
    # bodies of several sizes (POST must be sent exactly once, in origin-form)
    for size in (1, 5000, 400_000):
        bp = os.path.join(TMP, "sbody.bin"); open(bp, "wb").write(b"x" * size)
        rc, out, err = curl(["-x", f"http://127.0.0.1:{GW}", "--data-binary", f"@{bp}", T("/upload")])
        check(rc == 0 and json.loads(out)["body_bytes"] == size and json.loads(out)["method"] == "POST", (size, rc, out, err))
    # HEAD, a chunked response and a close-delimited one all work through the tunnel
    check(get(T("/chunked"))[0] == 200 and get(T("/close"))[0] == 200 and get(T("/health"), method="HEAD")[0] == 200)
    # CONNECT through a SOCKS5 upstream: tunnel established by the SOCKS5 handshake, then raw bytes
    c = open_tunnel(); c.send("GET /via-socks-connect HTTP/1.1\r\nHost: x\r\n\r\n")
    s, _, b = c.response(); check(s == 200 and b'"path":"/via-socks-connect"' in b, (s, b))
    check(metrics()["active_tunnels"] == 1); c.s.close(); time.sleep(0.3); check(metrics()["active_tunnels"] == 0)
    # concurrency over SOCKS5 with a handful of threads
    codes = []
    ts = [threading.Thread(target=lambda i=i: codes.append(get(T(f"/par/{i}"))[0])) for i in range(150)]
    [t.start() for t in ts]; [t.join(30) for t in ts]
    check(codes.count(200) == 150, (len(codes), set(codes))); check(thread_count(gw.pid) <= 12, thread_count(gw.pid))
    # the gateway really used SOCKS5: the same node declared as plain HTTP cannot work
    cleanup()
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET); start_node(PX[0], "socks5")
    start_gateway(gw_cfg(proxies=[(PX[0], "http")], max_retries=0))
    check(get(T("/should-fail"))[0] in (502, 503, 504), "HTTP spoken to a SOCKS5-only node must fail")
    cleanup()
    # ... and a node declared as socks5 but speaking HTTP only fails too (bad handshake, fast, no hang)
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET); start_node(PX[1], "http")
    start_gateway(gw_cfg(proxies=[(PX[1], "socks5")], max_retries=0))
    t0 = time.time(); check(get(T("/should-fail"))[0] in (502, 503, 504)); check(time.time() - t0 < 3)
    check(get(f"http://127.0.0.1:{TARGET}/x", method="GET")[0] in (502, 503, 504))
    cleanup()
    # invalid protocol value is rejected at startup
    p = os.path.join(TMP, "badproto.json")
    json.dump(gw_cfg(proxies=[(PX[0], "socks4")]), open(p, "w"))
    r = subprocess.run([f"{BUILD}/gateway", "--check-config", "--config", p], capture_output=True, text=True, timeout=10)
    check(r.returncode == 1 and "protocol" in (r.stdout + r.stderr), (r.returncode, r.stdout, r.stderr))


def test_mixed_protocols_failover_and_probes():
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET)
    n0 = start_node(PX[0], "socks5"); start_node(PX[1], "http")
    start_gateway(gw_cfg(proxies=[(PX[0], "socks5"), (PX[1], "http")], policy="round_robin"))
    for i in range(40): check(get(T(f"/mix/{i}"))[0] == 200, i)
    st = proxies_state(); check(st[0]["success_count"] >= 15 and st[1]["success_count"] >= 15, st)
    # the SOCKS5 proxy dies: idempotent requests fail over to the HTTP proxy
    stop(n0, signal.SIGKILL); time.sleep(0.2)
    check(all(get(T(f"/after-kill/{i}"))[0] == 200 for i in range(20)), "failover SOCKS5 -> HTTP")
    cleanup()
    # health probes go through the SOCKS5 handshake too (probe interval 200 ms)
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET); start_node(PX[0], "socks5")
    start_gateway(gw_cfg(proxies=[(PX[0], "socks5")], timeouts={"probe_interval_ms": 200}, circuit={"failure_threshold": 2, "cooldown_sec": 1}))
    time.sleep(1.5); check(proxies_state()[0]["healthy"], proxies_state())
    cleanup()
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET); start_node(PX[0], "socks5")
    start_gateway(gw_cfg(proxies=[(PX[0], "http")], timeouts={"probe_interval_ms": 200}, circuit={"failure_threshold": 2, "cooldown_sec": 60}))
    time.sleep(1.5); check(not proxies_state()[0]["healthy"], "probe over the wrong protocol must mark the proxy unhealthy")


def test_socks5_username_password():
    SECRET = "s3cr3t-P@ss"
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET)
    start_node(PX[0], "socks5", auth=f"alice:{SECRET}")
    gw = start_gateway(gw_cfg(proxies=[(PX[0], "socks5", "alice", SECRET)], timeouts={"probe_interval_ms": 200}))
    # requests, bodies, a CONNECT tunnel, concurrency: all through an authenticated SOCKS5 proxy
    for i in range(30): check(get(T(f"/auth/{i}"))[0] == 200, i)
    bp = os.path.join(TMP, "abody.bin"); open(bp, "wb").write(b"y" * 70000)
    rc, out, err = curl(["-x", f"http://127.0.0.1:{GW}", "--data-binary", f"@{bp}", T("/upload")])
    check(rc == 0 and json.loads(out)["body_bytes"] == 70000, (rc, out, err))
    c = open_tunnel(); c.send("GET /auth-tunnel HTTP/1.1\r\nHost: x\r\n\r\n")
    s_, _, b = c.response(); check(s_ == 200 and b'"path":"/auth-tunnel"' in b, (s_, b)); c.s.close()
    codes = []
    ts = [threading.Thread(target=lambda i=i: codes.append(get(T(f"/p/{i}"))[0])) for i in range(100)]
    [t.start() for t in ts]; [t.join(30) for t in ts]
    check(codes.count(200) == 100, (len(codes), set(codes)))
    time.sleep(1.0); check(proxies_state()[0]["healthy"], "probes must authenticate too")
    # credentials never leak through the admin endpoints
    for path in ("/proxies", "/metrics", "/health"):
        check(SECRET.encode() not in get(path)[2] and b"alice" not in get(path)[2], path)
    cleanup()

    def broken(creds, probe_ms=60000):
        start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET)
        start_node(PX[0], "socks5", auth=f"alice:{SECRET}")
        return start_gateway(gw_cfg(proxies=[(PX[0], "socks5") + creds], max_retries=0,
                                    timeouts={"probe_interval_ms": probe_ms}))
    # wrong password -> clean, fast 502 (the proxy rejects the sub-negotiation), never a hang
    broken(("alice", "wrong")); t0 = time.time()
    check(get(T("/x"))[0] in (502, 503) and time.time() - t0 < 3)
    cleanup()
    # no credentials configured but the proxy requires them
    broken(()); check(get(T("/x"))[0] in (502, 503)); cleanup()
    # wrong credentials: the health prober marks the proxy unhealthy
    broken(("alice", "wrong"), probe_ms=200); time.sleep(1.5)
    check(not proxies_state()[0]["healthy"], proxies_state()); cleanup()
    # credentials configured, proxy needs none: the "no authentication" method is chosen, traffic flows
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET); start_node(PX[0], "socks5")
    start_gateway(gw_cfg(proxies=[(PX[0], "socks5", "alice", SECRET)]))
    check(get(T("/open-proxy"))[0] == 200); cleanup()
    # mixed: authenticated SOCKS5 proxy dies, HTTP proxy takes over
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET)
    n0 = start_node(PX[0], "socks5", auth=f"alice:{SECRET}"); start_node(PX[1], "http")
    start_gateway(gw_cfg(proxies=[(PX[0], "socks5", "alice", SECRET), (PX[1], "http")], policy="round_robin"))
    check(all(get(T(f"/m/{i}"))[0] == 200 for i in range(20)))
    stop(n0, signal.SIGKILL); time.sleep(0.2)
    check(all(get(T(f"/f/{i}"))[0] == 200 for i in range(10)), "failover from authenticated SOCKS5 to HTTP")
    cleanup()
    # config validation
    def cfg_err(entry):
        c = gw_cfg(proxies=[PX[0]]); c["proxies"] = [dict({"host": "h", "port": 1}, **entry)]
        pth = os.path.join(TMP, "authcfg.json"); json.dump(c, open(pth, "w"))
        r = subprocess.run([f"{BUILD}/gateway", "--check-config", "--config", pth], capture_output=True, text=True, timeout=10)
        return r.returncode, r.stdout + r.stderr
    rc, o = cfg_err({"protocol": "http", "username": "a", "password": "b"}); check(rc == 1 and "socks5" in o, o)
    rc, o = cfg_err({"protocol": "socks5", "username": "a"}); check(rc == 1 and "together" in o, o)
    rc, o = cfg_err({"protocol": "socks5", "username": "", "password": "b"}); check(rc == 1 and "1..255" in o, o)
    rc, o = cfg_err({"protocol": "socks5", "username": "a", "password": "p" * 256}); check(rc == 1 and "1..255" in o, o)
    rc, o = cfg_err({"protocol": "socks5", "username": "a", "password": "p" * 255}); check(rc == 0, o)


def test_admin_token_and_secret_sources():
    start_bed(((PX[0], 0, 0.0),))
    tok = "tok-9f3a1c7e5b"
    start_gateway(gw_cfg(proxies=[PX[0]], admin={"token": tok}))
    bearer = f"Authorization: Bearer {tok}\r\n"
    def raw(path, hdr=""):
        c = Conn(); c.send(f"GET {path} HTTP/1.1\r\nHost: x\r\n{hdr}Connection: close\r\n\r\n"); return c.response()
    for path in ("/metrics", "/proxies"):
        st, h, _ = raw(path); check(st == 401 and "bearer" in h.get("www-authenticate", "").lower(), (path, st, h))
        check(raw(path, "Authorization: Bearer wrong-token-xx\r\n")[0] == 401)
        check(raw(path, f"Authorization: {tok}\r\n")[0] == 401, "scheme is required")
        check(raw(path, bearer)[0] == 200, path)
    st, _, body = raw("/health"); check(st == 200 and b"healthy" in body and b"aimd" not in body, body)   # open but minimal
    check(get(T("/proxied"))[0] == 200, "proxy traffic needs no token")
    check(raw("/nope", bearer)[0] == 404)
    cleanup()
    # token from an environment variable / a file; the password for a SOCKS5 proxy likewise
    pwfile = os.path.join(TMP, "pw.txt"); open(pwfile, "w").write("file-pass-123\n")
    tkfile = os.path.join(TMP, "tok.txt"); open(tkfile, "w").write("file-token-4567\n")
    os.chmod(pwfile, 0o600); os.chmod(tkfile, 0o600)
    os.environ["GW_E2E_PASS"] = "env-pass-456"; os.environ["GW_E2E_TOKEN"] = "env-token-890"
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET)
    start_node(PX[0], "socks5", auth="alice:env-pass-456"); start_node(PX[1], "socks5", auth="bob:file-pass-123")
    cfg = gw_cfg(proxies=[PX[0], PX[1]], policy="round_robin", admin={"token_env": "GW_E2E_TOKEN"})
    cfg["proxies"] = [{"host": "127.0.0.1", "port": PX[0], "protocol": "socks5", "username": "alice", "password_env": "GW_E2E_PASS"},
                      {"host": "127.0.0.1", "port": PX[1], "protocol": "socks5", "username": "bob", "password_file": pwfile}]
    start_gateway(cfg)
    check(all(get(T(f"/s/{i}"))[0] == 200 for i in range(20)), "both proxies authenticate with externally stored secrets")
    st = json.loads(raw("/proxies", "Authorization: Bearer env-token-890\r\n")[2]); check(all(x["success_count"] > 0 for x in st), st)
    cleanup()
    start_bed(((PX[0], 0, 0.0),)); cfg = gw_cfg(proxies=[PX[0]], admin={"token_file": tkfile}); start_gateway(cfg)
    check(raw("/metrics", "Authorization: Bearer file-token-4567\r\n")[0] == 200 and raw("/metrics")[0] == 401)
    cleanup()
    # check-config: unset env var / unreadable file / two sources / short token are rejected, with clear messages
    def chk(entry, admin=None, mode=None):
        c = gw_cfg(proxies=[PX[0]]); c["proxies"] = [dict({"host": "h", "port": 1, "protocol": "socks5", "username": "u"}, **entry)]
        if admin is not None: c["admin"] = admin
        pth = os.path.join(TMP, "sec.json"); json.dump(c, open(pth, "w"))
        if mode is not None: os.chmod(pth, mode)
        r = subprocess.run([f"{BUILD}/gateway", "--check-config", "--config", pth], capture_output=True, text=True, timeout=10)
        return r.returncode, r.stdout + r.stderr
    rc, o = chk({"password_env": "GW_E2E_UNSET_VAR"}); check(rc == 1 and "GW_E2E_UNSET_VAR" in o, o)
    rc, o = chk({"password_file": "/nonexistent/pw"}); check(rc == 1 and "cannot read" in o, o)
    rc, o = chk({"password": "a", "password_file": pwfile}); check(rc == 1 and "only one" in o, o)
    rc, o = chk({"password": "ok"}, admin={"token": "short"}); check(rc == 1 and "8..1024" in o, o)
    # world-readable config with an inline password warns; 0600 and external secrets do not
    rc, o = chk({"password": "inline-secret"}, mode=0o644); check(rc == 0 and "WARNING" in o and "inline-secret" not in o, o)
    rc, o = chk({"password": "inline-secret"}, mode=0o600); check(rc == 0 and "WARNING" not in o, o)
    rc, o = chk({"password_file": pwfile}, mode=0o644); check(rc == 0 and "WARNING" not in o, o)
    os.chmod(pwfile, 0o644)
    rc, o = chk({"password_file": pwfile}, mode=0o600); check(rc == 0 and "secret file" in o and pwfile in o and "file-pass-123" not in o, o)
    os.chmod(pwfile, 0o600)


def _ipv6_available():
    try:
        sk = socket.socket(socket.AF_INET6, socket.SOCK_STREAM); sk.bind(("::1", 0)); sk.close(); return True
    except OSError:
        return False


def test_ipv6_end_to_end():
    if not _ipv6_available(): raise Skip("no IPv6 on this host")
    start([f"{BUILD}/target_server", "--port", str(TARGET), "--listen", "::1"], TARGET)   # origin reachable ONLY over IPv6
    start([f"{BUILD}/proxy_node", "--port", str(PX[0]), "--listen", "::1", "--only", "socks5"], PX[0])
    start([f"{BUILD}/proxy_node", "--port", str(PX[1]), "--listen", "::1", "--only", "http"], PX[1])
    cfg = gw_cfg(proxies=[PX[0]], policy="round_robin", listen_host="::1")
    cfg["proxies"] = [{"host": "::1", "port": PX[0], "protocol": "socks5"}, {"host": "::1", "port": PX[1], "protocol": "http"}]
    start_gateway(cfg)
    for i in range(10):
        rc, out, err = curl(["-g", "-x", f"http://[::1]:{GW}", f"http://[::1]:{TARGET}/v6/{i}"])
        check(rc == 0 and json.loads(out)["path"] == f"/v6/{i}", (rc, out, err))
    st = json.loads(curl(["-g", f"http://[::1]:{GW}/proxies"])[1]); check(all(x["success_count"] > 0 for x in st), st)
    check(st[0]["endpoint"].startswith("[::1]:"), st)
    # CONNECT to an IPv6 literal through the SOCKS5 proxy (ATYP 4) and through the HTTP proxy
    for _ in range(2):
        c = socket.create_connection(("::1", GW), timeout=10); c.sendall(f"CONNECT [::1]:{TARGET} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
        check(c.recv(4096).startswith(b"HTTP/1.1 200"), "CONNECT to an IPv6 literal"); c.close()


def _non_loopback_ip():
    try:
        out = subprocess.run(["hostname", "-I"], capture_output=True, text=True, timeout=5).stdout.split()
    except Exception:
        return None
    for ip in out:
        if ":" not in ip and not ip.startswith("127."): return ip
    return None


def _http(host, port, path, hdr="", method="GET"):
    c = socket.create_connection((host, port), timeout=10)
    c.sendall(f"{method} {path} HTTP/1.1\r\nHost: x\r\n{hdr}Connection: close\r\n\r\n".encode())
    data = b""
    while True:
        d = c.recv(65536)
        if not d: break
        data += d
    c.close()
    head, _, body = data.partition(b"\r\n\r\n")
    return int(head.split()[1]), head.decode("latin1").lower(), body


def test_admin_interface_access_control():
    ip = _non_loopback_ip()
    # 1) default: admin paths on the proxy port answer LOOPBACK clients only
    start_bed(((PX[0], 0, 0.0),)); start_gateway(gw_cfg(proxies=[PX[0]], listen_host="0.0.0.0"))
    check(_http("127.0.0.1", GW, "/metrics")[0] == 200)
    if ip:
        st, _, body = _http(ip, GW, "/metrics"); check(st == 403 and b"localhost" in body, (st, body))
        check(_http(ip, GW, "/proxies")[0] == 403 and _http(ip, GW, "/health")[0] == 403)
        c = socket.create_connection((ip, GW), timeout=10)      # ... but the PROXY service itself is still open to them
        c.sendall(f"GET {T('/from-remote')} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n".encode())
        check(c.recv(4096).startswith(b"HTTP/1.1 200")); c.close()
    cleanup()
    # 2) allow_remote needs a token; with one, remote clients must present it
    if ip:
        start_bed(((PX[0], 0, 0.0),)); start_gateway(gw_cfg(proxies=[PX[0]], listen_host="0.0.0.0", admin={"allow_remote": True, "token": "remote-token-123"}))
        check(_http(ip, GW, "/metrics")[0] == 401)
        check(_http(ip, GW, "/metrics", "Authorization: Bearer remote-token-123\r\n")[0] == 200)
        check(_http(ip, GW, "/health")[0] == 200); cleanup()
    # 3) dedicated admin listener: bound to 127.0.0.1, serves ONLY admin paths; the proxy port serves none
    AP = 18900
    start_bed(((PX[0], 0, 0.0),)); start_gateway(gw_cfg(proxies=[PX[0]], listen_host="0.0.0.0", admin={"port": AP}))
    check(_http("127.0.0.1", AP, "/metrics")[0] == 200 and _http("127.0.0.1", AP, "/proxies")[0] == 200)
    st, _, body = _http("127.0.0.1", GW, "/metrics"); check(st == 404 and b"separate port" in body, (st, body))
    check(_http("127.0.0.1", AP, T("/x"))[0] == 403, "the admin port must never proxy")
    c = socket.create_connection(("127.0.0.1", AP), timeout=10); c.sendall(f"CONNECT 127.0.0.1:{TARGET} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
    check(c.recv(4096).startswith(b"HTTP/1.1 403")); c.close()
    check(get(T("/still-proxied"))[0] == 200)
    if ip: check(subprocess.run(["curl", "-s", "-m", "2", "-o", "/dev/null", "--noproxy", "*", f"http://{ip}:{AP}/metrics"]).returncode != 0, "not reachable on the external interface")
    cleanup()

    # 4) config validation: exposure without a token is refused; bad combinations rejected
    def chk(admin, listen_port=None):
        c = gw_cfg(proxies=[PX[0]], admin=admin)
        if listen_port: c["listen_port"] = listen_port
        pth = os.path.join(TMP, "adm.json"); json.dump(c, open(pth, "w"))
        r = subprocess.run([f"{BUILD}/gateway", "--check-config", "--config", pth], capture_output=True, text=True, timeout=10)
        return r.returncode, r.stdout + r.stderr
    rc, o = chk({"port": 9001, "listen_host": "0.0.0.0"}); check(rc == 1 and "token" in o and "gen-admin-token" in o, o)
    rc, o = chk({"allow_remote": True}); check(rc == 1 and "token" in o, o)
    rc, o = chk({"port": GW}); check(rc == 1 and "must differ" in o, o)
    rc, o = chk({"port": 9001, "listen_host": "admin.example"}); check(rc == 1 and "listen_host" in o, o)
    rc, o = chk({"port": 9001, "listen_host": "::1"}); check(rc == 0 and "WARNING" not in o, o)
    rc, o = chk({"port": 9001, "listen_host": "0.0.0.0", "token": "a-long-enough-token"})
    check(rc == 0 and "[Security] WARNING" in o and "unencrypted" in o and "a-long-enough-token" not in o, o)


def test_generated_admin_token():
    out = subprocess.run([f"{BUILD}/gateway", "--gen-admin-token"], capture_output=True, text=True, timeout=10).stdout.strip()
    check(len(out) == 64 and all(ch in "0123456789abcdef" for ch in out), out)
    check(out != subprocess.run([f"{BUILD}/gateway", "--gen-admin-token"], capture_output=True, text=True, timeout=10).stdout.strip(), "must be random")
    f = os.path.join(TMP, "gen.tok")
    if os.path.exists(f): os.remove(f)
    old = os.umask(0o000)       # even with a permissive umask the file must be private
    try: r = subprocess.run([f"{BUILD}/gateway", "--gen-admin-token", f], capture_output=True, text=True, timeout=10)
    finally: os.umask(old)
    check(r.returncode == 0 and (os.stat(f).st_mode & 0o777) == 0o600, (r.stdout, r.stderr, oct(os.stat(f).st_mode)))
    tok = open(f).read().strip(); check(len(tok) == 64 and tok not in r.stdout, "the token must not be echoed when written to a file")
    r2 = subprocess.run([f"{BUILD}/gateway", "--gen-admin-token", f], capture_output=True, text=True, timeout=10)
    check(r2.returncode == 1 and open(f).read().strip() == tok, "never overwrites an existing file")
    start_bed(((PX[0], 0, 0.0),)); start_gateway(gw_cfg(proxies=[PX[0]], admin={"port": 18901, "token_file": f}))
    check(_http("127.0.0.1", 18901, "/metrics")[0] == 401 and _http("127.0.0.1", 18901, "/metrics", f"Authorization: Bearer {tok}\r\n")[0] == 200)


def test_untrusted_socks5_warnings():
    def chk(host, proto="socks5", creds=True):
        c = gw_cfg(proxies=[PX[0]]); e = {"host": host, "port": 1080, "protocol": proto}
        if creds: e.update(username="alice", password="Sup3r-S3cret-PW")
        c["proxies"] = [e]
        pth = os.path.join(TMP, "sw.json"); json.dump(c, open(pth, "w")); os.chmod(pth, 0o600)
        r = subprocess.run([f"{BUILD}/gateway", "--check-config", "--config", pth], capture_output=True, text=True, timeout=10)
        return r.returncode, r.stdout + r.stderr
    for host in ("8.8.8.8", "172.32.0.1", "100.128.0.1", "2001:db8::1", "::ffff:8.8.8.8"):
        rc, o = chk(host); check(rc == 0 and "[Security] WARNING" in o and "UNENCRYPTED" in o and "username/password" in o, (host, o))
        check("Sup3r-S3cret-PW" not in o and "alice" not in o, "credentials must never be printed")
    rc, o = chk("8.8.8.8", creds=False); check("unauthenticated" in o and "UNENCRYPTED" in o, o)
    for host in ("127.0.0.1", "10.1.2.3", "172.16.0.9", "172.31.255.254", "192.168.1.5", "169.254.9.9", "100.64.0.1", "100.127.255.254",
                 "::1", "fd12::1", "fe80::1", "::ffff:10.0.0.1"):
        rc, o = chk(host); check(rc == 0 and "WARNING" not in o, (host, o))
    rc, o = chk("8.8.8.8", proto="http", creds=False); check(rc == 0 and "WARNING" not in o, "only SOCKS5 is flagged")


def test_zz_no_secret_ever_logged():
    secrets = ["s3cr3t-P@ss", "env-pass-456", "file-pass-123", "inline-secret", "tok-9f3a1c7e5b", "env-token-890", "file-token-4567",
               "remote-token-123", "Sup3r-S3cret-PW", "a-long-enough-token", "from-env-secret", "from-file-secret"]
    hits = []
    for name in os.listdir(TMP):
        pth = os.path.join(TMP, name)
        if name.endswith(".log") and os.path.isfile(pth):       # every gateway / proxy_node / target_server log of this run
            txt = open(pth, "rb").read()
            hits += [(name, sct) for sct in secrets if sct.encode() in txt]
    check(not hits, f"secrets leaked into logs: {hits}")


def test_idle_upstream_connections_are_reaped():
    start_bed(((PX[0], 0, 0.0),))
    gw = start_gateway(gw_cfg(proxies=[PX[0]], workers={"min": 4, "max": 2000, "initial": 1000, "io_threads": 2},
                              timeouts={"upstream_idle_ms": 600}))
    nfd = lambda: len(os.listdir(f"/proc/{gw.pid}/fd"))
    cpu = lambda: sum(int(x) for x in open(f"/proc/{gw.pid}/stat").read().rsplit(")", 1)[1].split()[11:13])
    base = nfd()
    ts = [threading.Thread(target=lambda i=i: get(T(f"/slow?ms=400"))) for i in range(120)]    # 120 simultaneous upstream connections
    [t.start() for t in ts]; [t.join(30) for t in ts]
    peak = nfd()
    check(peak >= base + 60, f"the pool should hold many idle upstream connections right after the burst (base {base}, now {peak})")
    deadline = time.time() + 6
    while nfd() > base + 6 and time.time() < deadline: time.sleep(0.2)
    check(nfd() <= base + 6, f"idle pooled connections must be closed by the reaper (base {base}, now {nfd()})")
    # an empty pool means no timers: the I/O threads sleep completely (no periodic wake-ups)
    c0 = cpu(); time.sleep(2.5); check(cpu() - c0 <= 3, f"idle CPU ticks: {cpu() - c0}")
    check(get(T("/still-works"))[0] == 200 and thread_count(gw.pid) <= 12)
    # a proxy that closes idle connections must not leave them in CLOSE_WAIT inside the gateway
    cleanup()
    start_bed(((PX[0], 0, 0.0),))
    gw = start_gateway(gw_cfg(proxies=[PX[0]], timeouts={"upstream_idle_ms": 60000}))   # long idle limit: only the liveness check can free them
    base = nfd()
    ts = [threading.Thread(target=lambda: get(T("/slow?ms=200"))) for _ in range(40)]
    [t.start() for t in ts]; [t.join(30) for t in ts]
    pn = [p for p in PROCS if p.args[0].endswith("proxy_node")][0]
    stop(pn, signal.SIGKILL)                                   # the proxy dies: all pooled connections are now dead
    deadline = time.time() + 12
    while nfd() > base + 6 and time.time() < deadline: time.sleep(0.2)
    check(nfd() <= base + 6, f"connections closed by the peer must be released promptly (base {base}, now {nfd()})")


def _no_proxy_env():
    return {k: v for k, v in os.environ.items() if k.lower() not in ("http_proxy", "https_proxy", "all_proxy", "no_proxy")}


def test_default_listener_is_loopback_only():
    ip = _non_loopback_ip()
    # no listen_host at all: the proxy port must default to 127.0.0.1 (an unauthenticated forward proxy is an open proxy)
    start_bed(((PX[0], 0, 0.0),))
    cfg = gw_cfg(proxies=[PX[0]]); del cfg["listen_host"]
    start_gateway(cfg)
    c = socket.create_connection(("127.0.0.1", GW), timeout=5); c.close()
    if ip:
        try:
            socket.create_connection((ip, GW), timeout=2).close(); raise AssertionError(f"default listener reachable from {ip}")
        except OSError:
            pass
    def warns(host):
        c2 = gw_cfg(proxies=[PX[0]], listen_host=host); pth = os.path.join(TMP, "lh.json"); json.dump(c2, open(pth, "w")); os.chmod(pth, 0o600)
        r = subprocess.run([f"{BUILD}/gateway", "--check-config", "--config", pth], capture_output=True, text=True, timeout=10)
        return r.stdout + r.stderr
    o = warns("0.0.0.0"); check("open proxy" in o and "NO client authentication" in o, o)
    o = warns("::"); check("open proxy" in o, o)
    for h in ("127.0.0.1", "::1"):
        o = warns(h); check("open proxy" not in o, (h, o))
    # explicit opt-in works
    cleanup(); start_bed(((PX[0], 0, 0.0),)); start_gateway(gw_cfg(proxies=[PX[0]], listen_host="0.0.0.0"))
    if ip: socket.create_connection((ip, GW), timeout=3).close()


def _with_hosts(entries, fn):
    """Temporarily add /etc/hosts lines (needs root); skipped otherwise."""
    path = "/etc/hosts"
    if not os.access(path, os.W_OK): raise Skip("/etc/hosts is not writable")
    orig = open(path).read()
    try:
        open(path, "w").write(orig.rstrip("\n") + "\n" + "\n".join(entries) + "\n")
        return fn()
    finally:
        open(path, "w").write(orig)


def test_dns_multi_address_failover():
    # "multi.gwtest" resolves to TWO addresses (glibc returns 127.0.0.1 first, then 127.0.0.2). With the backends bound to
    # 127.0.0.2 the FIRST address refuses, so requests, probes and CONNECT tunnels must fall over to the second one.
    # (This is the localhost = ::1 first, then 127.0.0.1 situation, reproducible without IPv6.) The reverse layout
    # (backends on 127.0.0.1) must keep working too.
    for backend in ("127.0.0.2", "127.0.0.1"):
        order = ("127.0.0.1", "127.0.0.2")
        def run():
            start([f"{BUILD}/target_server", "--port", str(TARGET), "--listen", backend], TARGET)
            start([f"{BUILD}/proxy_node", "--port", str(PX[0]), "--listen", backend], PX[0])
            cfg = gw_cfg(proxies=[PX[0]], timeouts={"probe_interval_ms": 300})
            cfg["proxies"] = [{"host": "multi.gwtest", "port": PX[0]}]
            cfg["probe_target_url"] = f"http://multi.gwtest:{TARGET}/health"
            start_gateway(cfg)
            for i in range(8):
                s, _, b = get(f"http://multi.gwtest:{TARGET}/fo/{i}")
                check(s == 200 and json.loads(b)["path"] == f"/fo/{i}", (backend, s, b))
            time.sleep(1.0)
            st = proxies_state()[0]; check(st["healthy"] and st["failure_count"] == 0 and st["success_count"] >= 8, (backend, st))
            for _ in range(3):   # CONNECT: proxy_node itself has to fail over (connect_with_timeout)
                c = socket.create_connection(("127.0.0.1", GW), timeout=10)
                c.sendall(f"CONNECT multi.gwtest:{TARGET} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
                check(c.recv(4096).startswith(b"HTTP/1.1 200"), ("CONNECT", backend)); c.close()
        _with_hosts([f"{order[0]} multi.gwtest", f"{order[1]} multi.gwtest"], run)
        cleanup()


def test_pool_limits_fd_budget_and_metrics():
    # 1) pool limits are configurable and respected; the idle count and fd usage are exported
    start_bed(((PX[0], 0, 0.0),))
    start_gateway(gw_cfg(proxies=[PX[0]], limits={"pool_max_idle_per_proxy": 3, "pool_max_idle_total": 5},
                         workers={"min": 4, "max": 500, "initial": 200}, timeouts={"upstream_idle_ms": 30000}))
    ts = [threading.Thread(target=lambda: get(T("/slow?ms=300"))) for _ in range(60)]
    [t.start() for t in ts]; [t.join(30) for t in ts]
    time.sleep(0.3)
    m = metrics()
    check(m["upstream_idle_connections"] <= 3, m["upstream_idle_connections"])
    check(0 < m["process_open_fds"] < m["process_fd_limit"], (m["process_open_fds"], m["process_fd_limit"]))
    check(get(T("/ok"))[0] == 200)
    cleanup()
    # pool_max_idle_per_proxy = 0 turns pooling off completely
    start_bed(((PX[0], 0, 0.0),)); start_gateway(gw_cfg(proxies=[PX[0]], limits={"pool_max_idle_per_proxy": 0}))
    for _ in range(10): check(get(T("/np"))[0] == 200)
    time.sleep(0.2); check(metrics()["upstream_idle_connections"] == 0)
    cleanup()
    # 2) the configured limits are clamped to RLIMIT_NOFILE instead of running into EMFILE
    import resource
    start_bed(((PX[0], 0, 0.0),))
    cfg = gw_cfg(proxies=[PX[0]], limits={"max_client_connections": 100000, "pool_max_idle_total": 4096, "pool_max_idle_per_proxy": 1000})
    pth = os.path.join(TMP, "fd.json"); json.dump(cfg, open(pth, "w"))
    lim = 300
    log = open(os.path.join(TMP, "gateway.fd.log"), "w")
    gw = subprocess.Popen([f"{BUILD}/gateway", "--config", pth], stdout=log, stderr=subprocess.STDOUT,
                          preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (lim, lim)))
    PROCS.append(gw); wait_port(GW)
    log.flush(); out = open(log.name).read()
    check("[Limits]" in out and "limits.max_client_connections" in out and "limits.pool_max_idle_total" in out, out)
    check(metrics()["process_fd_limit"] == lim, metrics())
    # open far more client connections than descriptors exist: the gateway must stay up, never exceed the limit
    socks = []
    for _ in range(lim + 150):
        try: socks.append(socket.create_connection(("127.0.0.1", GW), timeout=2))
        except OSError: break
    time.sleep(0.5)
    check(gw.poll() is None, "gateway died under fd pressure")
    check(len(os.listdir(f"/proc/{gw.pid}/fd")) <= lim, "fd limit exceeded")
    for s in socks: s.close()
    time.sleep(0.5)
    check(get(T("/after-pressure"))[0] == 200, "gateway must recover once the connections are gone")


def test_probe_sends_correct_host_header():
    # The health probe is a real request through the proxy: Host must be the probe URL's authority (not a made-up name),
    # otherwise real proxies / CDN-fronted sites reject it. Also any 2xx counts as healthy (204 generate_204 style URLs).
    seen = []
    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            seen.append((self.path, self.headers.get("Host"), self.headers.get("User-Agent")))
            self.send_response(204 if self.path.startswith("/generate_204") else 200); self.send_header("Content-Length", "0"); self.end_headers()
        def log_message(self, *a): pass
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", TARGET), H); threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        for path in ("/health", "/generate_204"):
            cleanup() if False else None
            seen.clear()
            gw_cfg_ = gw_cfg(proxies=[PX[0]], timeouts={"probe_interval_ms": 200}); gw_cfg_["probe_target_url"] = f"http://127.0.0.1:{TARGET}{path}"
            start([f"{BUILD}/proxy_node", "--port", str(PX[0])], PX[0]); gw = start_gateway(gw_cfg_)
            time.sleep(1.2)
            check(seen, "the probe never reached the origin")
            check(all(h == f"127.0.0.1:{TARGET}" for _, h, _ in seen), seen[:3])
            st = proxies_state()[0]; check(st["healthy"] and st["success_count"] >= 1 and st["failure_count"] == 0, (path, st))
            for p_ in list(PROCS): stop(p_)
            PROCS.clear(); time.sleep(0.3)
    finally:
        srv.shutdown(); srv.server_close()


def test_probe_failure_reasons_are_reported():
    # /proxies.last_error must say WHY a proxy is failing (the control panel shows it to the user)
    start([f"{BUILD}/target_server", "--port", str(TARGET)], TARGET)
    start_node(PX[0], only="socks5", auth="alice:pw")
    s5 = {"host": "127.0.0.1", "port": PX[0], "protocol": "socks5"}

    def probe_result(proxy):
        for q in [q for q in PROCS if q.args[0].endswith("gateway")]:
            stop(q); PROCS.remove(q)
        cfg = gw_cfg(proxies=[PX[1]], timeouts={"probe_interval_ms": 150, "connect_ms": 1000})
        cfg["proxies"] = [proxy]
        start_gateway(cfg)
        deadline, st = time.time() + 6, {}
        while time.time() < deadline:
            st = proxies_state()[0]
            if st["last_error"] or st["success_count"]: break
            time.sleep(0.1)
        return st

    st = probe_result(s5);                                            check("requires authentication" in st["last_error"], st)
    st = probe_result(dict(s5, username="alice", password="WRONG"));  check("wrong username or password" in st["last_error"], st)
    st = probe_result(dict(s5, username="alice", password="pw"));     check(st["last_error"] == "" and st["success_count"] >= 1, st)
    st = probe_result({"host": "127.0.0.1", "port": 1});              check("cannot connect to the proxy" in st["last_error"], st)
    st = probe_result(dict(s5, protocol="http"));                     check(st["last_error"] != "" and st["failure_count"] > 0, st)


class Skip(Exception):
    pass


def test_kernel_tune_script():
    shim = os.path.join(TMP, "shim"); state = os.path.join(TMP, "sysctl_state")
    os.makedirs(shim, exist_ok=True); os.makedirs(state, exist_ok=True)
    init = {"net.core.somaxconn": "128", "net.ipv4.tcp_tw_reuse": "2", "net.ipv4.ip_local_port_range": "32768\t60999",
            "net.ipv4.tcp_fin_timeout": "60", "net.core.netdev_max_backlog": "1000"}
    for k, v in init.items(): open(os.path.join(state, k), "w").write(v)
    open(os.path.join(shim, "sysctl"), "w").write(f"""#!/bin/bash
S="{state}"
if [ "$1" = "-n" ]; then cat "$S/$2"; echo; exit 0; fi
if [ "$1" = "-w" ]; then k="${{2%%=*}}"; v="${{2#*=}}"; printf '%s' "$v" > "$S/$k"; echo "$k = $v"; exit 0; fi
exit 1
""")
    open(os.path.join(shim, "sudo"), "w").write('#!/bin/bash\nexec "$@"\n')
    for f in ("sysctl", "sudo"): os.chmod(os.path.join(shim, f), 0o755)
    env = dict(os.environ, PATH=shim + ":" + os.environ["PATH"], BACKUP_FILE=os.path.join(TMP, "sysctl.bak"))
    script = os.path.join(ROOT, "scripts", "kernel_tune.sh")
    run = lambda a: subprocess.run(["bash", script, a], env=env, capture_output=True, text=True)
    val = lambda k: open(os.path.join(state, k)).read()
    r = run("apply"); check(r.returncode == 0, r.stderr)
    check(val("net.core.somaxconn") == "4096" and val("net.ipv4.ip_local_port_range") == "10240 65535", "applied")
    r = run("apply"); check(r.returncode == 1, "second apply must refuse (would overwrite the saved originals)")
    check(val("net.core.somaxconn") == "4096")
    r = run("revert"); check(r.returncode == 0, r.stderr)
    for k, v in init.items(): check(val(k).split() == v.split(), (k, val(k)))
    check(not os.path.exists(env["BACKUP_FILE"]), "backup removed after revert")
    check(run("revert").returncode == 1, "revert without backup")
    check(run("bogus").returncode == 1)


def test_docker_files_consistent():
    try: import yaml
    except ImportError: raise Skip("pyyaml missing")
    comp = yaml.safe_load(open(os.path.join(ROOT, "docker-compose.yml")))
    svcs = comp["services"]
    cfgs = set()
    for name, s in svcs.items():
        cmd = s["command"].split() if isinstance(s["command"], str) else s["command"]
        for a in cmd:
            if a.endswith(".json"):
                check(os.path.exists(os.path.join(ROOT, a)), f"{name}: missing {a}"); cfgs.add(a)
    check(cfgs == {"config/gateway.docker.json"}, cfgs)
    cfg = json.load(open(os.path.join(ROOT, "config", "gateway.docker.json")))
    ports = {}
    for name, s in svcs.items():
        cmd = s["command"].split()
        if "--port" in cmd: ports[name] = int(cmd[cmd.index("--port") + 1])
    for p in cfg["proxies"]:
        check(p["host"] in svcs and ports[p["host"]] == p["port"], f"proxy {p} must match a compose service/port")
    check(cfg["probe_target_url"] == f"http://target:{ports['target']}/health")
    check(not any("127.0.0.1" in json.dumps(p) for p in cfg["proxies"]), "no loopback upstreams inside containers")
    # hostname-based upstreams really resolve and work (stand-in for compose DNS)
    start_bed()
    c = gw_cfg(proxies=[]); c["proxies"] = [{"host": "localhost", "port": p} for p in PX]
    start_gateway(c)
    check(all(get(T(f"/dns/{i}"))[0] == 200 for i in range(10)), "hostname upstreams")
    if shutil.which("docker") and subprocess.run(["docker", "compose", "version"], capture_output=True).returncode == 0:
        env = dict(os.environ, GATEWAY_ADMIN_TOKEN="test-token-1234567890")
        r = subprocess.run(["docker", "compose", "-f", os.path.join(ROOT, "docker-compose.yml"), "config", "-q"], capture_output=True, text=True, cwd=ROOT, env=env)
        check(r.returncode == 0, r.stderr)
        r = subprocess.run(["docker", "compose", "-f", os.path.join(ROOT, "docker-compose.yml"), "config", "-q"], capture_output=True, text=True, cwd=ROOT,
                           env={k: v for k, v in os.environ.items() if k != "GATEWAY_ADMIN_TOKEN"})
        check(r.returncode != 0, "compose must refuse to start without an admin token")


def test_docker_full_stack():
    if not ARGS.docker: raise Skip("pass --docker to run")
    cf = os.path.join(ROOT, "docker-compose.yml")
    tok = "docker-test-token-" + os.urandom(8).hex()
    env = dict(os.environ, GATEWAY_ADMIN_TOKEN=tok)
    subprocess.run(["docker", "compose", "-f", cf, "up", "--build", "-d"], check=True, cwd=ROOT, env=env)
    try:
        wait_port(8000, 60); time.sleep(3)
        rc, out, _ = curl(["-x", "http://127.0.0.1:8000", "http://target:9000/products/1"])
        check(rc == 0 and b"product" in out or rc == 0, (rc, out))
        # admin interface: dedicated port on the host loopback, token required; not served on the proxy port
        wait_port(8001, 30)
        check(_http("127.0.0.1", 8001, "/metrics")[0] == 401)
        check(_http("127.0.0.1", 8001, "/metrics", f"Authorization: Bearer {tok}\r\n")[0] == 200)
        check(_http("127.0.0.1", 8000, "/metrics")[0] in (403, 404))
    finally:
        subprocess.run(["docker", "compose", "-f", cf, "down", "--rmi", "local", "-v"], cwd=ROOT, env=env)


# --------------------------------------------------------------------------- runner
def main():
    global BUILD, ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=BUILD)
    ap.add_argument("--only", default="")
    ap.add_argument("--docker", action="store_true")
    ARGS = ap.parse_args()
    BUILD = os.path.abspath(ARGS.build)
    for b in ("gateway", "proxy_node", "target_server"):
        if not os.path.exists(f"{BUILD}/{b}"):
            sys.exit(f"missing {BUILD}/{b}: build the project first")
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_") and callable(f) and ARGS.only in n]
    tests.sort(key=lambda t: list(globals()).index(t[0]))
    failed = 0
    for name, fn in tests:
        t0 = time.time()
        try:
            fn(); print(f"[PASS] {name} ({time.time() - t0:.1f}s)")
        except Skip as e:
            print(f"[SKIP] {name}: {e}")
        except Exception:
            failed += 1
            print(f"[FAIL] {name} ({time.time() - t0:.1f}s)\n" + "".join("    " + l for l in traceback.format_exc().splitlines(True)))
            for lf in sorted(os.listdir(TMP)):
                if lf.endswith(".log") and "gateway" in lf:
                    print(f"    --- {lf} ---\n" + open(os.path.join(TMP, lf)).read()[-800:])
        finally:
            cleanup()
    shutil.rmtree(TMP, ignore_errors=True)
    print(f"\n{len(tests) - failed}/{len(tests)} e2e tests passed" + ("" if not failed else f", {failed} FAILED"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
