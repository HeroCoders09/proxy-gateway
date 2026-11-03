#!/usr/bin/env python3
"""Network-level fuzzing / hostile-peer test for the gateway (stdlib only).

    python3 scripts/fuzz_network.py --build build [--rounds 3] [--seed 1]

Best run against a sanitizer build (-fsanitize=address,undefined) with ASAN_OPTIONS=log_path=... so memory errors surface.

Phase 1  hostile UPSTREAM HTTP proxy: ~20 misbehaviours (garbage, truncation, bad chunking, trailers, 1xx floods, huge
         headers, endless bodies, stalls, RSTs, dribbling, ...). Each has an expected outcome class.
Phase 2  hostile UPSTREAM SOCKS5 proxy: bad versions/methods/replies, split & truncated replies, every reply code,
         username/password failures, stalls, bytes before the client speaks, ... plus CONNECT through it.
Phase 3  hostile CLIENTS: mutated requests, random bytes, huge/many headers, pipelining floods, RSTs, half-open, storms.
After every phase the gateway must be alive, answer a normal request, and have returned its fds/threads to baseline.
"""
import argparse, json, os, random, signal, socket, struct, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GW, TARGET, GOOD = 18300, 19300, 18301          # gateway, origin, a healthy proxy_node (control)
HP, SP = 18310, 18311                           # hostile HTTP proxy, hostile SOCKS5 proxy
TMP = tempfile.mkdtemp(prefix="gw_fuzz_")
PROCS = []
FAILS = []


def log(*a): print(*a, flush=True)


def fail(msg):
    FAILS.append(msg); log("   FAIL:", msg)


# ----------------------------------------------------------------------------------------- process helpers
def start(args, port, name):
    f = open(os.path.join(TMP, name + ".log"), "w")
    p = subprocess.Popen(args, stdout=f, stderr=subprocess.STDOUT)
    PROCS.append(p)
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.3).close(); return p
        except OSError: time.sleep(0.1)
    raise RuntimeError(f"{name} did not start")


def nfds(p): return len(os.listdir(f"/proc/{p.pid}/fd"))
def nthreads(p): return len(os.listdir(f"/proc/{p.pid}/task"))


def http(port, req, timeout=15, host="127.0.0.1"):
    """One request on a fresh connection; returns (status or None, seconds)."""
    t0 = time.time()
    try:
        c = socket.create_connection((host, port), timeout=timeout)
        c.settimeout(timeout)
        c.sendall(req if isinstance(req, bytes) else req.encode())
        data = b""
        while b"\r\n\r\n" not in data:
            d = c.recv(65536)
            if not d: break
            data += d
        c.close()
        if data.startswith(b"HTTP/1."): return int(data.split()[1]), time.time() - t0
        return None, time.time() - t0
    except (OSError, ValueError):
        return None, time.time() - t0


def get(path, **kw): return http(GW, f"GET http://127.0.0.1:{TARGET}{path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", **kw)
def post(path, **kw): return http(GW, f"POST http://127.0.0.1:{TARGET}{path} HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nConnection: close\r\n\r\nabcd", **kw)


# ----------------------------------------------------------------------------------------- hostile servers
def rst(c):
    c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)); c.close()


def read_head(c, limit=70000):
    c.settimeout(3); data = b""
    try:
        while b"\r\n\r\n" not in data and len(data) < limit:
            d = c.recv(65536)
            if not d: break
            data += d
    except OSError: pass
    return data


OK_BODY = b'{"path":"/fuzz","ok":true}'
OK = b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n" % len(OK_BODY) + OK_BODY


def stall(c, secs=12):
    try: time.sleep(secs)
    except Exception: pass


def endless(c):
    read_head(c)
    try:
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n")
        t0 = time.time(); chunk = b"x" * 65536
        while time.time() - t0 < 10: c.sendall(chunk)
    except OSError: pass


def dribble(c):
    read_head(c)
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # otherwise Nagle + delayed ACK stretch it past the response deadline
    try:
        for i in range(len(OK)):
            c.sendall(OK[i:i + 1])
            if i < len(OK) - 1: time.sleep(0.004)    # no pause after the last byte: closing late would race with connection reuse
    except OSError: pass


def interim_flood(c):
    read_head(c)
    try: c.sendall(b"HTTP/1.1 100 Continue\r\n\r\n" * 1000 + OK)
    except OSError: pass


def reply(data, wait_for_request=True):
    def f(c):
        if wait_for_request: read_head(c)
        try: c.sendall(data)
        except OSError: pass
    return f


HTTP_BEHAVIOURS = {   # name: (handler, expected): "200" must be 200, "err" must be 502/503/504, "any" any of 200/5xx
    "ok": (reply(OK), "200"),
    "close": (lambda c: None, "err"),
    "rst": (rst, "err"),
    "garbage": (reply(os.urandom(500)), "err"),
    "partial_headers_stall": (lambda c: (read_head(c), c.sendall(b"HTTP/1.1 200 OK\r\nContent-Le"), stall(c)), "err"),
    "short_body": (reply(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789"), "err"),
    "bad_chunk": (reply(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\nZZ\r\n"), "err"),
    "chunk_trailers": (reply(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\nX-Trailer: 1\r\n\r\n"), "200"),
    "chunk_extra": (reply(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\nEXTRA"), "200"),
    "chunk_huge_size": (reply(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFF\r\nx"), "err"),
    "huge_header": (lambda c: (read_head(c), c.sendall(b"HTTP/1.1 200 OK\r\nX: " + b"a" * 1_500_000)), "err"),
    "dup_content_length": (reply(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 5\r\n\r\nabcde"), "err"),
    "negative_length": (reply(b"HTTP/1.1 200 OK\r\nContent-Length: -5\r\n\r\nabc"), "err"),
    "bad_status": (reply(b"HTTP/1.1 abc OK\r\n\r\n"), "err"),
    "status_999": (reply(b"HTTP/1.1 999 Odd\r\nContent-Length: 0\r\n\r\n"), "err"),
    "no_status_line": (reply(b"\r\n\r\n"), "err"),
    "endless": (endless, "err"),
    "dribble": (dribble, "200"),
    "early": (reply(OK, wait_for_request=False), "200"),
    "interim_flood": (interim_flood, "200"),
    "ok_then_close": (reply(OK), "200"),
    "nul_bytes": (reply(b"HTTP/1.1 200 OK\r\nX-A: \x00\x00\r\nContent-Length: 2\r\n\r\nok"), "any"),
}


def sock_read(c, n, timeout=3):
    c.settimeout(timeout); data = b""
    while len(data) < n:
        d = c.recv(n - len(data))
        if not d: raise OSError("eof")
        data += d
    return data


def socks_handshake(c, mode):
    """Server side of SOCKS5; returns True when the client should now be served as an origin."""
    ver, nm = sock_read(c, 2)
    methods = sock_read(c, nm)
    if mode == "stall_method": stall(c); return False
    if mode == "refuse": c.sendall(b"\x05\xff"); return False
    if mode == "badver": c.sendall(b"\x04\x00"); return False
    if mode == "garbage_method": c.sendall(os.urandom(2)); return False
    if mode == "unknown_method": c.sendall(b"\x05\x03"); return False
    if mode.startswith("auth"):
        if b"\x02" not in methods: c.sendall(b"\x05\xff"); return False
        c.sendall(b"\x05\x02")
        if mode == "auth_stall": stall(c); return False
        sock_read(c, 1); ul = sock_read(c, 1)[0]; u = sock_read(c, ul); pl = sock_read(c, 1)[0]; pw = sock_read(c, pl)
        if mode == "auth_fail": c.sendall(b"\x01\x01"); return False
        if mode == "auth_badver": c.sendall(b"\x05\x00"); return False
        if mode == "auth_truncated": c.sendall(b"\x01"); return False
        if mode == "auth_ok" and (u, pw) != (b"fuzz", b"pass123"): c.sendall(b"\x01\x01"); return False
        c.sendall(b"\x01\x00")
        mode = "ok"
    else:
        c.sendall(b"\x05\x00")
    hdr = sock_read(c, 4)
    atyp = hdr[3]
    if atyp == 1: sock_read(c, 4)
    elif atyp == 3: sock_read(c, sock_read(c, 1)[0])
    elif atyp == 4: sock_read(c, 16)
    sock_read(c, 2)
    if mode == "stall_reply": stall(c); return False
    if mode.startswith("fail_"): c.sendall(bytes([5, int(mode[5:]), 0, 1, 0, 0, 0, 0, 0, 0])); return False
    if mode == "truncated_reply": c.sendall(b"\x05\x00\x00\x01\x7f"); return False
    if mode == "bad_atyp": c.sendall(b"\x05\x00\x00\x09\x00\x00"); return False
    if mode == "badver_reply": c.sendall(b"\x04\x00\x00\x01\x00\x00\x00\x00\x00\x00"); return False
    if mode == "close_after_method": return False
    if mode == "rst_reply": rst(c); return False
    rep = b"\x05\x00\x00\x01\x7f\x00\x00\x01\x1f\x90"
    if mode == "split_reply":
        for i in range(len(rep)): c.sendall(rep[i:i + 1]); time.sleep(0.01)
    elif mode == "extra_after_reply": c.sendall(rep + b"JUNKJUNK")
    elif mode == "domain_reply": c.sendall(b"\x05\x00\x00\x03\x04host\x00\x50")
    elif mode == "v6_reply": c.sendall(b"\x05\x00\x00\x04" + b"\x00" * 16 + b"\x00\x50")
    else: c.sendall(rep)
    return True


SOCKS_MODES = {   # mode: expected for a plain GET ("200" / "err" / "any") and for CONNECT
    "ok": ("200", "200"), "split_reply": ("200", "200"), "domain_reply": ("200", "200"), "v6_reply": ("200", "200"),
    "extra_after_reply": ("err", "200"),            # a server must not speak first on a request; for CONNECT the bytes are tunnel data
    "refuse": ("err", "err"), "badver": ("err", "err"), "garbage_method": ("err", "err"), "unknown_method": ("err", "err"),
    "stall_method": ("err", "err"), "stall_reply": ("err", "err"), "truncated_reply": ("err", "err"), "bad_atyp": ("err", "err"),
    "badver_reply": ("err", "err"), "close_after_method": ("err", "err"), "rst_reply": ("err", "err"),
    **{f"fail_{i}": ("err", "err") for i in range(1, 9)},
    "auth_ok": ("200", "200"), "auth_fail": ("err", "err"), "auth_badver": ("err", "err"), "auth_truncated": ("err", "err"), "auth_stall": ("err", "err"),
}


class Hostile:
    def __init__(self, port, kind):
        self.kind, self.port, self.mode, self.connect_mode = kind, port, "ok", "ok"
        self.lsock = socket.socket(); self.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.lsock.bind(("127.0.0.1", port)); self.lsock.listen(512); self.running = True
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while self.running:
            try: c, _ = self.lsock.accept()
            except OSError: return
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    def serve(self, c):
        try:
            if self.kind == "http": self.serve_http(c)
            else: self.serve_socks(c)
        except Exception: pass
        finally:
            try: c.close()
            except Exception: pass

    def serve_http(self, c):
        if self.connect_mode != "-":
            head = read_head(c) if False else None
        # peek the first line without consuming: decide CONNECT vs plain
        c.settimeout(3)
        first = c.recv(8, socket.MSG_PEEK)
        if first.startswith(b"CONNECT"):
            read_head(c)
            m = self.connect_mode
            if m == "ok": c.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n"); tunnel_echo(c)
            elif m == "ok_leftover": c.sendall(b"HTTP/1.1 200 OK\r\n\r\nHELLO"); tunnel_echo(c)
            elif m == "refused": c.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            elif m == "garbage": c.sendall(os.urandom(200))
            elif m == "partial": c.sendall(b"HTTP/1.1 20"); stall(c)
            elif m == "stall": stall(c)
            elif m == "close": pass
            elif m == "rst": rst(c)
            elif m == "huge": c.sendall(b"HTTP/1.1 200 OK\r\nX: " + b"a" * 100000)
            elif m == "status_200x": c.sendall(b"HTTP/1.1 2000 OK\r\n\r\n")
            return
        HTTP_BEHAVIOURS[self.mode][0](c)

    def serve_socks(self, c):
        if socks_handshake(c, self.mode if self.mode in SOCKS_MODES else "ok"):
            read_first = c.recv(8, socket.MSG_PEEK) if False else None
            # origin role: answer one HTTP request (or echo bytes for CONNECT-style use)
            c.settimeout(3)
            try:
                data = c.recv(8, socket.MSG_PEEK)
            except OSError:
                return
            if data.startswith((b"GET ", b"POST", b"PUT ", b"HEAD", b"DELE")):
                read_head(c); c.sendall(OK)
            else:
                tunnel_echo(c)


def tunnel_echo(c):
    c.settimeout(3)
    try:
        while True:
            d = c.recv(4096)
            if not d: return
            c.sendall(d)
    except OSError: pass


# ----------------------------------------------------------------------------------------- gateway
def gw_config(proxies, **extra):
    cfg = {"listen_host": "127.0.0.1", "listen_port": GW, "policy": "round_robin", "max_retries": 0,
           "workers": {"min": 2, "max": 512, "initial": 256, "io_threads": 2},
           "timeouts": {"connect_ms": 500, "response_ms": 1500, "probe_interval_ms": 600000, "queue_ms": 30000,
                        "client_idle_ms": 3000, "client_header_ms": 2000, "tunnel_idle_ms": 3000},
           "limits": {"max_response_bytes": 2_000_000, "max_header_bytes": 16384, "max_body_bytes": 1_000_000},
           "circuit": {"failure_threshold": 1000000, "cooldown_sec": 1},
           "default_rate_limit": {"rate": 1000000, "burst": 1000000},
           "proxies": proxies, "probe_target_url": f"http://127.0.0.1:{TARGET}/health", "output": {"csv_path": "", "json_path": ""}}
    cfg.update(extra); return cfg


def start_gateway(build, proxies, **extra):
    path = os.path.join(TMP, f"gw{len(PROCS)}.json")
    json.dump(gw_config(proxies, **extra), open(path, "w"))
    return start([f"{build}/gateway", "--config", path], GW, f"gateway{len(PROCS)}")


def stop_gateway(p):
    p.send_signal(signal.SIGTERM)
    try: p.wait(15)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
    return p.returncode


def healthy(p, what):
    if p.poll() is not None: fail(f"{what}: gateway DIED (exit {p.returncode})"); return False
    return True


def settle(p, base_fd, base_thr, what, wait=8):
    """After the abuse the gateway must give resources back (idle timeouts are 2-3 s)."""
    deadline = time.time() + wait
    while time.time() < deadline and (nfds(p) > base_fd + 8 or nthreads(p) > base_thr + 2): time.sleep(0.3)
    if nfds(p) > base_fd + 8: fail(f"{what}: fd leak: {nfds(p)} open vs baseline {base_fd}")
    if nthreads(p) > base_thr + 2: fail(f"{what}: thread growth: {nthreads(p)} vs baseline {base_thr}")


ERR = (502, 503, 504)


def judge(expect, st, secs, what, limit):
    if st is None: fail(f"{what}: no HTTP response (connection dropped / hung) after {secs:.1f}s")
    elif secs > limit: fail(f"{what}: took {secs:.1f}s (> {limit}s)")
    elif expect == "200" and st != 200: fail(f"{what}: expected 200, got {st}")
    elif expect == "err" and st not in ERR: fail(f"{what}: expected 502/503/504, got {st}")
    elif expect == "any" and st != 200 and st not in ERR: fail(f"{what}: unexpected status {st}")


# ----------------------------------------------------------------------------------------- phases
def phase_http(build, rounds):
    log("== phase 1: hostile upstream HTTP proxy")
    hp = Hostile(HP, "http")
    gw = start_gateway(build, [{"host": "127.0.0.1", "port": HP}])
    time.sleep(0.3); get("/warm"); base_fd, base_thr = nfds(gw), nthreads(gw)
    for rnd in range(rounds):
        for name, (_, expect) in HTTP_BEHAVIOURS.items():
            hp.mode = name
            for method, fn in (("GET", get), ("POST", post)):
                if method == "POST" and name in ("early", "ok_then_close", "interim_flood", "chunk_extra"): continue   # POST is never retried / reused: same path
                st, secs = fn(f"/{name}")
                exp = expect
                if method == "POST" and expect == "200" and name in ("dribble",): exp = "200"
                judge(exp, st, secs, f"http/{name}/{method}", 7)
            if not healthy(gw, f"http/{name}"): return gw
        # CONNECT through the hostile proxy
        for cm, exp in (("ok", "200"), ("ok_leftover", "200"), ("refused", "err"), ("garbage", "err"), ("partial", "err"), ("stall", "err"),
                        ("close", "err"), ("rst", "err"), ("huge", "err"), ("status_200x", "err")):
            hp.connect_mode = cm
            st, secs = http(GW, f"CONNECT 127.0.0.1:{TARGET} HTTP/1.1\r\nHost: x\r\n\r\n")
            judge(exp, st, secs, f"http/CONNECT/{cm}", 7)
        if not healthy(gw, "http/CONNECT"): return gw
    settle(gw, base_fd, base_thr, "phase 1")
    hp.mode, hp.connect_mode = "ok", "ok"
    judge("200", *get("/after"), "phase 1: normal request afterwards", 5)
    hp.running = False; hp.lsock.close()
    return gw


def phase_socks(build, rounds):
    log("== phase 2: hostile upstream SOCKS5 proxy (with and without credentials)")
    sp = Hostile(SP, "socks5")
    gw = start_gateway(build, [{"host": "127.0.0.1", "port": SP, "protocol": "socks5", "username": "fuzz", "password": "pass123"}])
    time.sleep(0.3); sp.mode = "ok"; get("/warm"); base_fd, base_thr = nfds(gw), nthreads(gw)
    for rnd in range(rounds):
        for mode, (exp_get, exp_connect) in SOCKS_MODES.items():
            sp.mode = mode
            judge(exp_get, *get(f"/{mode}"), f"socks5/{mode}/GET", 7)
            st, secs = http(GW, f"CONNECT 127.0.0.1:{TARGET} HTTP/1.1\r\nHost: x\r\n\r\n")
            judge(exp_connect, st, secs, f"socks5/{mode}/CONNECT", 7)
            if not healthy(gw, f"socks5/{mode}"): return gw
            if mode.startswith("auth") is False and mode == "ok":
                pass
    settle(gw, base_fd, base_thr, "phase 2")
    sp.mode = "ok"; judge("200", *get("/after"), "phase 2: normal request afterwards", 5)
    sp.running = False; sp.lsock.close()
    return gw


def phase_clients(build, rounds, rng):
    log("== phase 3: hostile clients")
    gw = start_gateway(build, [{"host": "127.0.0.1", "port": GOOD}])
    time.sleep(0.3); get("/warm"); base_fd, base_thr = nfds(gw), nthreads(gw)
    seeds = [f"GET http://127.0.0.1:{TARGET}/a?b=c HTTP/1.1\r\nHost: x\r\n\r\n".encode(),
             f"POST http://127.0.0.1:{TARGET}/up HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello".encode(),
             f"POST http://127.0.0.1:{TARGET}/ch HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n".encode(),
             f"CONNECT 127.0.0.1:{TARGET} HTTP/1.1\r\nHost: x\r\n\r\n".encode(),
             b"GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n", b"GET /proxies HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer a\r\n\r\n",
             f"PUT http://127.0.0.1:{TARGET}/p HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\nabc".encode()]
    toks = [b"\r\n", b"\r\n\r\n", b"\n", b": ", b"Content-Length: ", b"Transfer-Encoding: chunked\r\n", b"\x00", b"\xff\xfe", b"%00", b"..", b"//", b"@",
            b"99999999999999999999", b"-1", b"HTTP/9.9", b"[::1]", b"http://", b"0\r\n\r\n", b"Connection: close\r\n", b"Expect: 100-continue\r\n"]

    def mutate(b):
        b = bytearray(b)
        for _ in range(rng.randint(1, 6)):
            k = rng.randrange(7)
            if k == 0 and b: b[rng.randrange(len(b))] ^= 1 << rng.randrange(8)
            elif k == 1 and b: b[rng.randrange(len(b))] = rng.randrange(256)
            elif k == 2: b[rng.randrange(len(b) + 1):0] = bytes([rng.randrange(256)])
            elif k == 3 and b: i = rng.randrange(len(b)); del b[i:i + rng.randint(1, 20)]
            elif k == 4: b[rng.randrange(len(b) + 1):0] = rng.choice(toks)
            elif k == 5 and b: del b[rng.randrange(len(b)):]
            elif k == 6: b[rng.randrange(len(b) + 1):0] = os.urandom(rng.randint(1, 200))
        return bytes(b)

    def abuse(payload, how):
        try:
            c = socket.create_connection(("127.0.0.1", GW), timeout=3)
            if how == "pieces":
                for i in range(0, len(payload), max(1, len(payload) // rng.randint(1, 4))):
                    c.sendall(payload[i:i + max(1, len(payload) // 3)]); time.sleep(rng.random() * 0.02)
            else:
                c.sendall(payload)
            c.settimeout(rng.choice([0.05, 0.3, 1.0]))
            try: c.recv(65536)
            except OSError: pass
            if how == "rst": rst(c)
            elif how == "halfclose":
                try: c.shutdown(socket.SHUT_WR)
                except OSError: pass
                time.sleep(0.05); c.close()
            else: c.close()
        except OSError: pass

    for rnd in range(rounds):
        for i in range(600):
            payload = os.urandom(rng.randint(0, 300)) if rng.random() < 0.15 else mutate(rng.choice(seeds))
            abuse(payload, rng.choice(["plain", "pieces", "rst", "halfclose"]))
            if i % 100 == 99 and not healthy(gw, f"clients/mutate{i}"): return gw
        # structural abuse
        big = lambda n: b"GET http://127.0.0.1:%d/x HTTP/1.1\r\nHost: x\r\n" % TARGET + b"".join(b"X-%d: v\r\n" % i for i in range(n)) + b"\r\n"
        for name, payload in (("5000 headers", big(5000)), ("1MB header value", b"GET http://127.0.0.1:%d/x HTTP/1.1\r\nHost: x\r\nX: " % TARGET + b"a" * 1_000_000 + b"\r\n\r\n"),
                              ("1MB url", b"GET http://127.0.0.1:%d/" % TARGET + b"a" * 1_000_000 + b" HTTP/1.1\r\nHost: x\r\n\r\n"),
                              ("huge Content-Length", b"POST http://127.0.0.1:%d/x HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999\r\n\r\n" % TARGET),
                              ("body over limit", b"POST http://127.0.0.1:%d/x HTTP/1.1\r\nHost: x\r\nContent-Length: 5000000\r\n\r\n" % TARGET + b"a" * 100000),
                              ("smuggle CL+TE", b"POST http://127.0.0.1:%d/x HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\nGET /x HTTP/1.1\r\n\r\n" % TARGET),
                              ("bare LF", b"GET http://127.0.0.1:%d/x HTTP/1.1\nHost: x\n\n" % TARGET),
                              ("NUL in header", b"GET http://127.0.0.1:%d/x HTTP/1.1\r\nHost: x\r\nX: a\x00b\r\n\r\n" % TARGET),
                              ("200 pipelined GETs", b"GET http://127.0.0.1:%d/p HTTP/1.1\r\nHost: x\r\n\r\n" % TARGET * 200),
                              ("pipeline then RST", b"GET http://127.0.0.1:%d/p HTTP/1.1\r\nHost: x\r\n\r\n" % TARGET * 50)):
            abuse(payload, "rst" if "RST" in name else "plain")
            if not healthy(gw, f"clients/{name}"): return gw
        # 100-continue without ever sending the body; slowloris herd; connect storm
        herd = []
        for _ in range(300):
            try:
                c = socket.create_connection(("127.0.0.1", GW), timeout=3)
                c.sendall(rng.choice([b"GET http://127.0.0.1:1/ HTT", b"POST http://127.0.0.1:1/ HTTP/1.1\r\nContent-Length: 10\r\nExpect: 100-continue\r\n\r\n", b""])); herd.append(c)
            except OSError: pass
        for _ in range(1500):
            try: socket.create_connection(("127.0.0.1", GW), timeout=2).close()
            except OSError: pass
        time.sleep(0.5)
        judge("200", *get(f"/during-herd-{rnd}"), "clients: normal request during a 300-connection slowloris herd", 5)
        for c in herd:
            try: c.close()
            except OSError: pass
        # many tunnels dropped abruptly
        for _ in range(100):
            try:
                c = socket.create_connection(("127.0.0.1", GW), timeout=3)
                c.sendall(f"CONNECT 127.0.0.1:{TARGET} HTTP/1.1\r\nHost: x\r\n\r\n".encode()); c.recv(100)
                c.sendall(b"GET /x HTTP/1.1\r\nHost: x\r\n\r\n")
                rst(c) if rng.random() < 0.5 else c.close()
            except OSError: pass
        if not healthy(gw, "clients/tunnels"): return gw
    settle(gw, base_fd, base_thr, "phase 3", wait=12)
    judge("200", *get("/after"), "phase 3: normal request afterwards", 5)
    return gw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    build = os.path.abspath(a.build)
    rng = random.Random(a.seed)
    signal.signal(signal.SIGPIPE, signal.SIG_IGN)
    start([f"{build}/target_server", "--port", str(TARGET)], TARGET, "target")
    start([f"{build}/proxy_node", "--port", str(GOOD)], GOOD, "good_proxy")
    t0 = time.time()
    try:
        for ph in (lambda: phase_http(build, a.rounds), lambda: phase_socks(build, a.rounds), lambda: phase_clients(build, a.rounds, rng)):
            gw = ph()
            rc = stop_gateway(gw)
            if rc not in (0, None): fail(f"gateway exit status {rc} after the phase (sanitizer abort / crash?)")
            time.sleep(0.5)
    finally:
        for p in PROCS:
            if p.poll() is None: p.kill()
    # sanitizer reports written into the gateway logs?
    for f in os.listdir(TMP):
        if f.endswith(".log"):
            txt = open(os.path.join(TMP, f), errors="replace").read()
            if "ERROR: AddressSanitizer" in txt or "runtime error:" in txt or "LeakSanitizer" in txt or "ThreadSanitizer" in txt:
                fail(f"sanitizer report in {f}:\n" + txt[:3000])
    log(f"\n{'ALL OK' if not FAILS else str(len(FAILS)) + ' FAILURE(S)'} in {time.time() - t0:.0f}s (logs in {TMP})")
    for f in FAILS: log(" -", f.splitlines()[0])
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
