#!/usr/bin/env python3
"""Proxy Gateway Control Panel: a desktop-style UI for the gateway (stdlib only).

Starts a small web server bound to 127.0.0.1 only, then opens it in an
app-style window (Chromium/Chrome/Edge --app mode, else the default browser).
Everything is done from the UI: start/stop, edit settings and proxies, live
metrics, logs, config validation, admin-token generation, test requests.

    python3 app/gateway_ui.py [--bin PATH] [--config PATH] [--no-open] [--keep-running]
"""
import argparse, copy, http.server, json, os, re, secrets, shutil, signal
import subprocess, sys, threading, time, urllib.error, urllib.request, webbrowser
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
HOME = Path.home()
CONF_DIR = Path(os.environ.get("XDG_CONFIG_HOME", HOME / ".config")) / "proxy-gateway"
STATE_DIR = Path(os.environ.get("XDG_STATE_HOME", HOME / ".local/state")) / "proxy-gateway"
SECRET_MASK = "•••set•••"   # shown instead of real secrets

DEFAULT_CONFIG = {
    "listen_port": 8000,
    "policy": "weighted",
    "max_queue_size": 10000,
    "max_retries": 2,
    "workers": {"min": 4, "max": 64, "initial": 16},
    "timeouts": {"connect_ms": 2000, "response_ms": 10000, "probe_interval_ms": 2000},
    "circuit": {"failure_threshold": 5, "cooldown_sec": 10},
    "default_rate_limit": {"rate": 200.0, "burst": 400.0},
    "rate_limits": {},
    "probe_target_url": "http://example.com/",   # fetched THROUGH each proxy every 2 s; must answer 200
    "proxies": [],
}


def find_binary(explicit):
    cands = [explicit, os.environ.get("GATEWAY_BIN"), ROOT / "build/gateway",
             ROOT / "bin/gateway", shutil.which("gateway")]
    for c in cands:
        if c and Path(c).is_file() and os.access(c, os.X_OK):
            return str(Path(c).resolve())
    return None


class Controller:
    def __init__(self, binary, config_path):
        self.bin = binary
        self.cfg_path = Path(config_path)
        self.log_path = STATE_DIR / "gateway.log"
        self.pid_path = STATE_DIR / "gateway.pid"
        self.proc = None
        self.adopted = None
        self.lock = threading.Lock()
        self.history = []          # (t, rps, p99, active) for the dashboard
        CONF_DIR.mkdir(parents=True, exist_ok=True, mode=0o700)
        STATE_DIR.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.cfg_path.parent.mkdir(parents=True, exist_ok=True)
        if not self.cfg_path.exists():
            self.write_config(DEFAULT_CONFIG)
        self._adopt()

    # ---- config -------------------------------------------------------
    def read_config(self):
        try:
            return json.loads(self.cfg_path.read_text())
        except Exception:
            return copy.deepcopy(DEFAULT_CONFIG)

    def write_config(self, cfg):
        tmp = self.cfg_path.with_suffix(".tmp")
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump(cfg, f, indent=2)
        os.replace(tmp, self.cfg_path)
        os.chmod(self.cfg_path, 0o600)

    @staticmethod
    def redact(cfg):
        cfg = copy.deepcopy(cfg)
        for p in cfg.get("proxies", []):
            if p.get("password"):
                p["password"] = SECRET_MASK
        adm = cfg.get("admin")
        if isinstance(adm, dict) and adm.get("token"):
            adm["token"] = SECRET_MASK
        return cfg

    def merge_secrets(self, new):
        """A masked secret from the UI means 'keep the stored value'."""
        old = self.read_config()
        old_pw = {(p.get("host"), p.get("port")): p.get("password") for p in old.get("proxies", [])}
        for p in new.get("proxies", []):
            if p.get("password") == SECRET_MASK:
                real = old_pw.get((p.get("host"), p.get("port")))
                if real:
                    p["password"] = real
            for k in ("username", "password", "password_env", "password_file"):
                if p.get(k) in ("", None, SECRET_MASK):
                    p.pop(k, None)
            if p.get("protocol") in ("", None, "http"):
                p.pop("protocol", None)
        adm = new.get("admin")
        if isinstance(adm, dict):
            if adm.get("token") == SECRET_MASK:
                real = (old.get("admin") or {}).get("token")
                if real:
                    adm["token"] = real
            for k in list(adm):
                if adm[k] in ("", None, SECRET_MASK):
                    del adm[k]
            if not adm:
                new.pop("admin")
        return new

    def check_config(self, cfg):
        """Validate with the gateway itself (--check-config) without saving."""
        if not self.bin:
            return False, "gateway binary not found"
        tmp = STATE_DIR / "check.json"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump(cfg, f)
        try:
            r = subprocess.run([self.bin, "--config", str(tmp), "--check-config"],
                               capture_output=True, text=True, timeout=15)
        finally:
            tmp.unlink(missing_ok=True)
        return r.returncode == 0, (r.stdout + r.stderr).strip()

    # ---- process ------------------------------------------------------
    def _adopt(self):
        try:
            pid = int(self.pid_path.read_text())
            cmd = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
            if cmd and Path(cmd[0].decode()).name == "gateway":
                self.adopted = pid
        except Exception:
            self.adopted = None

    def pid(self):
        if self.proc and self.proc.poll() is None:
            return self.proc.pid
        if self.adopted:
            try:
                os.kill(self.adopted, 0)
                return self.adopted
            except OSError:
                self.adopted = None
        return None

    def start(self):
        with self.lock:
            if self.pid():
                return False, "already running"
            if not self.bin:
                return False, "gateway binary not found (build the project first)"
            ok, out = self.check_config(self.read_config())
            if not ok:
                return False, "config invalid:\n" + out
            logf = open(self.log_path, "ab")
            logf.write(f"\n=== start {time.strftime('%F %T')} ===\n".encode())
            logf.flush()
            self.proc = subprocess.Popen([self.bin, "--config", str(self.cfg_path)],
                                         stdout=logf, stderr=subprocess.STDOUT,
                                         start_new_session=True)
            logf.close()
            self.adopted = None
            self.pid_path.write_text(str(self.proc.pid))
            time.sleep(0.4)
            if self.proc.poll() is not None:
                return False, "gateway exited immediately:\n" + self.tail_log(15)
            return True, "started"

    def stop(self):
        with self.lock:
            pid = self.pid()
            if not pid:
                return False, "not running"
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
            deadline = time.time() + 6
            while time.time() < deadline:
                if self.proc is not None:
                    if self.proc.poll() is not None:
                        break
                else:
                    try:
                        os.kill(pid, 0)
                    except OSError:
                        break
                time.sleep(0.1)
            else:
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
            if self.proc is not None:
                try:
                    self.proc.wait(2)
                except Exception:
                    pass
            self.proc = None
            self.adopted = None
            self.pid_path.unlink(missing_ok=True)
            return True, "stopped"

    def tail_log(self, n=200):
        try:
            data = self.log_path.read_bytes()[-200_000:].decode("utf-8", "replace")
        except Exception:
            return ""
        return "\n".join(data.splitlines()[-n:])

    # ---- talking to the running gateway ----------------------------------
    def admin_endpoint(self):
        cfg = self.read_config()
        adm = cfg.get("admin") or {}
        port = adm.get("port") or cfg.get("listen_port", 8000)
        host = adm.get("listen_host", "127.0.0.1") if adm.get("port") else "127.0.0.1"
        if host in ("0.0.0.0", "::", ""):
            host = "127.0.0.1"
        if ":" in host:
            host = f"[{host}]"
        token = adm.get("token")
        if not token and adm.get("token_file"):
            try:
                token = Path(adm["token_file"]).read_text().strip()
            except Exception:
                pass
        if not token and adm.get("token_env"):
            token = os.environ.get(adm["token_env"])
        return f"http://{host}:{port}", token

    def fetch(self, path, timeout=1.5):
        base, token = self.admin_endpoint()
        req = urllib.request.Request(base + path)
        if token:
            req.add_header("Authorization", "Bearer " + token)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(req, timeout=timeout) as r:
            return r.read().decode("utf-8", "replace")

    @staticmethod
    def parse_metrics(text):
        m, lat, per_proxy = {}, {}, {}
        for line in text.splitlines():
            if line.startswith("#") or not line.strip():
                continue
            g = re.match(r'^(\w+)(?:\{([^}]*)\})?\s+(\S+)$', line)
            if not g:
                continue
            name, labels, val = g.groups()
            try:
                v = float(val)
            except ValueError:
                continue
            if name == "latency_ms" and labels:
                q = re.search(r'quantile="([\d.]+)"', labels)
                if q:
                    lat[q.group(1)] = v
            elif labels:
                px = re.search(r'proxy="([^"]+)"', labels)
                if px:
                    per_proxy.setdefault(px.group(1), {})[name] = v
            else:
                m[name] = v
        m["latency"] = lat
        m["per_proxy"] = per_proxy
        return m

    def state(self):
        pid = self.pid()
        cfg = self.read_config()
        st = {"binary": self.bin, "running": bool(pid), "pid": pid, "reachable": False,
              "config_path": str(self.cfg_path), "config": self.redact(cfg),
              "listen_port": cfg.get("listen_port", 8000)}
        if pid:
            try:
                st["metrics"] = self.parse_metrics(self.fetch("/metrics"))
                st["proxies"] = json.loads(self.fetch("/proxies"))
                st["health"] = json.loads(self.fetch("/health"))
                st["reachable"] = True
                m = st["metrics"]
                self.history.append((time.time(), m.get("requests_per_second", 0),
                                     m["latency"].get("0.99", 0), m.get("active_connections", 0)))
                self.history = self.history[-120:]
            except Exception as e:
                st["error"] = str(e)
        else:
            self.history = []
        st["history"] = self.history
        return st

    def test_request(self, url, method, body):
        port = self.read_config().get("listen_port", 8000)
        if not re.match(r"^http://", url):
            return {"error": "Test URL must start with http:// (for https use: curl -x http://127.0.0.1:%d https://...)" % port}
        if not self.pid():
            return {"error": "gateway is not running"}
        t0 = time.time()
        try:
            # Talk to the gateway directly (absolute-form request line). urllib's
            # ProxyHandler would honour NO_PROXY and silently bypass the gateway.
            import http.client
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
            data = body.encode() if method == "POST" else None
            conn.request(method, url, body=data, headers={"Connection": "close"})
            r = conn.getresponse()
            payload = r.read(4000)
            out = {"status": r.status, "ms": round((time.time() - t0) * 1000, 1),
                   "headers": dict(r.getheaders()), "body": payload.decode("utf-8", "replace")}
            conn.close()
            return out
        except Exception as e:
            return {"error": str(e)}

    def gen_token(self):
        """Create a 0600 token file and point admin.token_file at it."""
        path = CONF_DIR / "admin.token"
        if path.exists():
            return False, f"{path} already exists (delete it first to rotate)"
        r = subprocess.run([self.bin, "--gen-admin-token", str(path)], capture_output=True, text=True)
        if r.returncode != 0:
            return False, (r.stdout + r.stderr).strip()
        cfg = self.read_config()
        adm = cfg.setdefault("admin", {})
        adm.pop("token", None)
        adm.pop("token_env", None)
        adm["token_file"] = str(path)
        self.write_config(cfg)
        return True, f"token written to {path} (mode 600) and set as admin.token_file"


class Handler(http.server.BaseHTTPRequestHandler):
    ctl = None
    session = None
    port = 0
    server_version = "GatewayUI"

    def log_message(self, *a):
        pass

    def _host_ok(self):
        host = (self.headers.get("Host") or "").lower()
        return host in (f"127.0.0.1:{self.port}", f"localhost:{self.port}")

    # security: loopback bind + Host check (DNS rebinding) + session cookie (CSRF)
    def _authorized(self):
        cookie = self.headers.get("Cookie") or ""
        return self._host_ok() and f"gwui={self.session}" in [c.strip() for c in cookie.split(";")]

    def _send(self, code, body, ctype="application/json", extra=None):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy",
                         "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj))

    def do_GET(self):
        if not self._host_ok():
            return self._send(403, "bad host", "text/plain")
        path, _, query = self.path.partition("?")
        if path == "/" and f"key={self.session}" in query:
            return self._send(302, "", "text/plain", {
                "Location": "/", "Set-Cookie": f"gwui={self.session}; HttpOnly; SameSite=Strict; Path=/"})
        if not self._authorized():
            return self._send(403, "open this app from its launcher", "text/plain")
        if path == "/":
            return self._send(200, (HERE / "index.html").read_bytes(), "text/html; charset=utf-8")
        if path == "/icon.svg":
            return self._send(200, (HERE / "proxy-gateway.svg").read_bytes(), "image/svg+xml")
        if path == "/api/state":
            return self._json(self.ctl.state())
        if path == "/api/logs":
            return self._json({"log": self.ctl.tail_log(400)})
        self._send(404, "not found", "text/plain")

    def do_POST(self):
        if not self._authorized():
            return self._send(403, "forbidden", "text/plain")
        n = int(self.headers.get("Content-Length") or 0)
        if n > 2_000_000:
            return self._json({"ok": False, "message": "too large"}, 413)
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
        except Exception:
            return self._json({"ok": False, "message": "bad json"}, 400)
        c, p = self.ctl, self.path
        if p == "/api/start":
            ok, msg = c.start()
            return self._json({"ok": ok, "message": msg})
        if p == "/api/stop":
            ok, msg = c.stop()
            return self._json({"ok": ok, "message": msg})
        if p == "/api/restart":
            c.stop()
            ok, msg = c.start()
            return self._json({"ok": ok, "message": msg})
        if p in ("/api/config", "/api/validate"):
            cfg = c.merge_secrets(body.get("config") or {})
            ok, out = c.check_config(cfg)
            if p == "/api/validate" or not ok:
                return self._json({"ok": ok, "message": out})
            c.write_config(cfg)
            msg = "saved"
            if body.get("restart") and c.pid():
                c.stop()
                ok, m2 = c.start()
                msg = "saved and restarted" if ok else "saved, but restart failed: " + m2
            elif c.pid():
                msg = "saved (restart the gateway to apply)"
            return self._json({"ok": ok, "message": msg})
        if p == "/api/gen-token":
            ok, msg = c.gen_token()
            return self._json({"ok": ok, "message": msg})
        if p == "/api/test":
            return self._json(c.test_request(str(body.get("url", "")), body.get("method", "GET"),
                                             str(body.get("body", ""))))
        if p == "/api/clear-log":
            c.log_path.write_bytes(b"")
            return self._json({"ok": True, "message": "cleared"})
        self._send(404, "not found", "text/plain")


def open_window(url):
    for exe in ("chromium", "chromium-browser", "google-chrome", "google-chrome-stable",
                "microsoft-edge", "brave-browser"):
        path = shutil.which(exe)
        if path:
            subprocess.Popen([path, f"--app={url}", "--window-size=1180,800",
                              f"--user-data-dir={STATE_DIR / 'window-profile'}"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return True
    return webbrowser.open(url)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", help="path to the gateway binary")
    ap.add_argument("--config", default=str(CONF_DIR / "gateway.json"))
    ap.add_argument("--port", type=int, default=0, help="UI port (default: random free port)")
    ap.add_argument("--no-open", action="store_true", help="do not open a window; print the URL")
    ap.add_argument("--keep-running", action="store_true", help="leave the gateway running when the UI exits")
    a = ap.parse_args()

    ctl = Controller(find_binary(a.bin), a.config)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    Handler.ctl, Handler.session, Handler.port = ctl, secrets.token_urlsafe(24), srv.server_address[1]
    url = f"http://127.0.0.1:{Handler.port}/?key={Handler.session}"
    print("Proxy Gateway Control Panel:", url, flush=True)
    if not ctl.bin:
        print("WARNING: gateway binary not found; build the project or pass --bin", file=sys.stderr)
    if not a.no_open:
        open_window(url)

    def bye(*_):
        threading.Thread(target=srv.shutdown, daemon=True).start()
    signal.signal(signal.SIGINT, bye)
    signal.signal(signal.SIGTERM, bye)
    try:
        srv.serve_forever()
    finally:
        if not a.keep_running and ctl.pid():
            ctl.stop()


if __name__ == "__main__":
    main()
