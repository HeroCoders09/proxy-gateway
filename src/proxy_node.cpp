// Controlled upstream proxy used for testing the gateway:
//   * HTTP/1.1 forward proxy (absolute-URI requests, request bodies, keep-alive, upstream connection reuse)
//   * CONNECT tunnels (HTTPS)
//   * SOCKS5 (no-auth, CONNECT command, IPv4 / domain addresses)
//   * artificial delay (--delay-ms) and random failure injection (--fail-rate)
#include "http.hpp"
#include "net.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <random>
#include <thread>
#include <unordered_map>

using namespace gateway;

struct NodeConfig {
    uint16_t port = 8101;
    std::string listen = "0.0.0.0";   // "::" = IPv6 + IPv4 (dual stack)
    int delay_ms = 0;
    double fail_rate = 0.0;
    std::string auth_user, auth_pass;   // --auth user:pass -> SOCKS5 requires RFC 1929 username/password
    bool auth = false;
    std::string only;   // "" = both protocols, "http" or "socks5" = refuse the other one (for tests)
};

static std::atomic<int> g_connections{0};
static constexpr int kMaxConnections = 8192;
static constexpr int kClientIdleMs = 60000;

// Applies the configured delay, then decides whether this request should fail.
static bool inject_and_should_fail(const NodeConfig& cfg) {
    thread_local std::mt19937 rng(std::random_device{}());
    if (cfg.delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(cfg.delay_ms));
    if (cfg.fail_rate <= 0.0) return false;
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng) < cfg.fail_rate;
}

static bool recv_exact(int fd, unsigned char* buf, size_t n, int timeout_ms = 10000) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv_some(fd, reinterpret_cast<char*>(buf) + got, n - got, timeout_ms);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// ------------------------------------------------------------------- SOCKS5
static void handle_socks5(int fd, const NodeConfig& cfg) {
    unsigned char hdr[2];
    if (!recv_exact(fd, hdr, 2) || hdr[0] != 0x05) return;
    unsigned char methods[256];
    if (hdr[1] > 0 && !recv_exact(fd, methods, hdr[1])) return;
    bool no_auth = false;
    for (int i = 0; i < hdr[1]; ++i) if (methods[i] == 0x00) no_auth = true;
    bool userpass = false;
    for (int i = 0; i < hdr[1]; ++i) if (methods[i] == 0x02) userpass = true;
    // With --auth the node insists on username/password; without it, it only accepts "no authentication".
    unsigned char method = cfg.auth ? (userpass ? 0x02 : 0xFF) : (no_auth ? 0x00 : 0xFF);
    unsigned char choice[2] = {0x05, method};
    send_all(fd, reinterpret_cast<char*>(choice), 2, 5000);
    if (method == 0xFF) return;
    if (method == 0x02) {
        unsigned char ver = 0, ulen = 0, plen = 0;
        std::string u, p;
        if (!recv_exact(fd, &ver, 1) || ver != 0x01 || !recv_exact(fd, &ulen, 1)) return;
        u.resize(ulen);
        if (ulen && !recv_exact(fd, reinterpret_cast<unsigned char*>(u.data()), ulen)) return;
        if (!recv_exact(fd, &plen, 1)) return;
        p.resize(plen);
        if (plen && !recv_exact(fd, reinterpret_cast<unsigned char*>(p.data()), plen)) return;
        bool ok = (u == cfg.auth_user && p == cfg.auth_pass);
        unsigned char ar[2] = {0x01, static_cast<unsigned char>(ok ? 0x00 : 0x01)};
        send_all(fd, reinterpret_cast<char*>(ar), 2, 5000);
        if (!ok) return;
    }

    unsigned char req[4];
    if (!recv_exact(fd, req, 4) || req[0] != 0x05) return;
    std::string host;
    unsigned char atyp = req[3];
    if (atyp == 0x01) {
        unsigned char ip[4];
        if (!recv_exact(fd, ip, 4)) return;
        char b[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, ip, b, sizeof(b));
        host = b;
    } else if (atyp == 0x03) {
        unsigned char len = 0;
        if (!recv_exact(fd, &len, 1) || len == 0) return;
        host.resize(len);
        if (!recv_exact(fd, reinterpret_cast<unsigned char*>(host.data()), len)) return;
    } else if (atyp == 0x04) {
        unsigned char ip[16];
        if (!recv_exact(fd, ip, 16)) return;
        char b[INET6_ADDRSTRLEN];
        ::inet_ntop(AF_INET6, ip, b, sizeof(b));
        host = b;
    }
    unsigned char pbuf[2];
    if (!recv_exact(fd, pbuf, 2)) return;
    uint16_t port = static_cast<uint16_t>((pbuf[0] << 8) | pbuf[1]);

    auto reply = [&](unsigned char code) {
        unsigned char r[10] = {0x05, code, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
        send_all(fd, reinterpret_cast<char*>(r), 10, 5000);
    };
    if (req[1] != 0x01) { reply(0x07); return; }          // only CONNECT is supported
    if (inject_and_should_fail(cfg)) { reply(0x01); return; }

    bool timed_out = false;
    int up = connect_with_timeout(host, port, 5000, timed_out);
    if (up < 0) { reply(timed_out ? 0x04 : 0x05); return; }
    reply(0x00);
    relay_bidirectional(fd, up, kClientIdleMs, nullptr);
    ::close(up);
}

// --------------------------------------------------------------------- HTTP
static void send_error(int fd, int status, const std::string& msg) {
    send_all(fd, build_response(status, "text/plain", msg + "\n", false), 5000);
}

static void handle_http(int fd, const NodeConfig& cfg) {
    std::string buf;
    std::unordered_map<std::string, int> upstream;  // authority -> reusable connection to the origin
    auto close_upstreams = [&]() { for (auto& kv : upstream) ::close(kv.second); upstream.clear(); };

    for (;;) {
        ParseResult pr;
        for (;;) {
            pr = parse_http_request(buf, 65536, 256u * 1024 * 1024);
            if (pr.status != ParseStatus::NeedMore) break;
            char tmp[16384];
            ssize_t n = recv_some(fd, tmp, sizeof(tmp), kClientIdleMs);
            if (n <= 0) { close_upstreams(); return; }
            buf.append(tmp, static_cast<size_t>(n));
        }
        if (pr.status == ParseStatus::Bad) { send_error(fd, pr.error_status, pr.error); close_upstreams(); return; }
        buf.erase(0, pr.consumed);
        HttpRequest& req = pr.req;

        if (inject_and_should_fail(cfg)) {
            send_error(fd, 502, "Injected Error");
            close_upstreams();
            return;
        }

        if (req.method == "CONNECT") {
            std::string host;
            uint16_t port = 0;
            if (!parse_authority(req.target, host, port, -1)) { send_error(fd, 400, "bad CONNECT target"); break; }
            bool timed_out = false;
            int up = connect_with_timeout(host, port, 5000, timed_out);
            if (up < 0) { send_error(fd, timed_out ? 504 : 502, "Target Down"); break; }
            send_all(fd, "HTTP/1.1 200 Connection Established\r\n\r\n", 5000);
            if (!buf.empty()) send_all(up, buf, 5000);  // bytes the client already sent after CONNECT
            relay_bidirectional(fd, up, kClientIdleMs, nullptr);
            ::close(up);
            close_upstreams();
            return;
        }

        UrlParts u;
        if (!parse_absolute_url(req.target, u) || u.https) { send_error(fd, 400, "absolute http:// URL required"); break; }

        std::string out = req.method + " " + u.path + " HTTP/1.1\r\n";
        bool has_host = false;
        for (const auto& kv : req.headers) {
            std::string ln = to_lower(kv.first);
            if (is_hop_by_hop(ln)) continue;
            if (ln == "host") has_host = true;
            out += kv.first + ": " + kv.second + "\r\n";
        }
        if (!has_host) out += "Host: " + u.authority + "\r\n";
        bool body_method = (req.method == "POST" || req.method == "PUT" || req.method == "PATCH");
        if (!req.body.empty() || body_method) out += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
        out += "Connection: keep-alive\r\n\r\n" + req.body;

        ResponseData rd;
        bool done = false;
        for (int attempt = 0; attempt < 2 && !done; ++attempt) {
            int up = -1;
            bool reused = false;
            auto it = upstream.find(u.authority);
            if (attempt == 0 && it != upstream.end()) { up = it->second; upstream.erase(it); reused = true; }
            else {
                if (it != upstream.end()) { ::close(it->second); upstream.erase(it); }
                bool timed_out = false;
                up = connect_with_timeout(u.host, u.port, 5000, timed_out);
                if (up < 0) { send_error(fd, timed_out ? 504 : 502, "Target Down"); close_upstreams(); return; }
            }
            if (!send_all(up, out, 10000)) { ::close(up); if (reused) continue; send_error(fd, 502, "Target write failed"); close_upstreams(); return; }
            ReadStatus rs = read_http_response(up, req.method == "HEAD", 30000, 512u * 1024 * 1024, rd);
            if (rs != ReadStatus::Ok) {
                ::close(up);
                if (reused && !rd.any_bytes) continue;  // stale pooled connection
                send_error(fd, rs == ReadStatus::Timeout ? 504 : 502, "Target error");
                close_upstreams();
                return;
            }
            if (rd.framed && !rd.close_header && !rd.peer_closed && !rd.extra_bytes) upstream[u.authority] = up;
            else ::close(up);
            done = true;
        }
        if (!done) { send_error(fd, 502, "Target error"); close_upstreams(); return; }

        if (!send_all(fd, rd.raw, 30000)) break;
        if (!req.keep_alive || !rd.framed || rd.close_header) break;
    }
    close_upstreams();
}

static void handle_client(int fd, NodeConfig cfg) {
    set_nodelay(fd);
    char first = 0;
    ssize_t n = 0;
    auto deadline = deadline_in(kClientIdleMs);
    for (;;) {  // peek the first byte to tell SOCKS5 (0x05) from HTTP
        n = ::recv(fd, &first, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n != -1 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
        if (wait_fd_until(fd, POLLIN, deadline) != 1) break;
    }
    if (n > 0) {
        bool is_socks = static_cast<unsigned char>(first) == 0x05;
        if (!cfg.only.empty() && is_socks != (cfg.only == "socks5")) { /* protocol disabled: just hang up */ }
        else if (is_socks) handle_socks5(fd, cfg);
        else handle_http(fd, cfg);
    }
    ::close(fd);
    g_connections.fetch_sub(1);
}

int main(int argc, char* argv[]) {
    NodeConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        try {
            if (arg == "--port" && i + 1 < argc) cfg.port = static_cast<uint16_t>(std::stoi(argv[++i]));
            else if (arg == "--delay-ms" && i + 1 < argc) cfg.delay_ms = std::stoi(argv[++i]);
            else if (arg == "--fail-rate" && i + 1 < argc) cfg.fail_rate = std::stod(argv[++i]);
            else if (arg == "--listen" && i + 1 < argc) cfg.listen = argv[++i];
            else if (arg == "--auth" && i + 1 < argc) {
                std::string v = argv[++i];
                size_t c = v.find(':');
                if (c == std::string::npos || c == 0 || c + 1 >= v.size()) { std::cerr << "--auth expects user:pass\n"; return 1; }
                cfg.auth = true; cfg.auth_user = v.substr(0, c); cfg.auth_pass = v.substr(c + 1);
            }
            else if (arg == "--only" && i + 1 < argc) {
                cfg.only = argv[++i];
                if (cfg.only != "http" && cfg.only != "socks5") { std::cerr << "--only must be http or socks5\n"; return 1; }
            }
            else { std::cerr << "usage: proxy_node [--port N] [--delay-ms N] [--fail-rate 0..1] [--only http|socks5] [--auth user:pass] [--listen ADDR]\n"; return 1; }
        } catch (const std::exception&) {
            std::cerr << "invalid value for " << arg << "\n";
            return 1;
        }
    }
    std::signal(SIGPIPE, SIG_IGN);

    int lfd = listen_on(cfg.listen, cfg.port, 2048);
    if (lfd < 0) {
        std::perror("proxy_node bind/listen");
        return 1;
    }
    std::cout << "[ProxyNode] Listening on port " << cfg.port << " (delay=" << cfg.delay_ms
              << "ms, fail_rate=" << cfg.fail_rate << ")" << std::endl;
    for (;;) {
        int cfd = ::accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) continue;
        if (g_connections.fetch_add(1) >= kMaxConnections) { ::close(cfd); g_connections.fetch_sub(1); continue; }
        std::thread(handle_client, cfd, cfg).detach();
    }
}
