// Deterministic origin server for tests. Supports keep-alive, request bodies (incl. chunked) and:
//   /health            -> 200 "ok" JSON
//   /slow?ms=N         -> responds after N ms
//   /status/N          -> responds with status N
//   /chunked           -> chunked response
//   /close             -> response delimited by connection close (no Content-Length)
//   /big?bytes=N       -> N bytes of payload
//   anything else      -> JSON echo {"status","method","path","body_bytes"}
#include "http.hpp"
#include "net.hpp"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <thread>

using namespace gateway;

static std::atomic<int> g_connections{0};

static std::string query_value(const std::string& target, const std::string& key) {
    size_t q = target.find('?');
    if (q == std::string::npos) return "";
    std::string qs = target.substr(q + 1);
    std::string needle = key + "=";
    size_t p = qs.find(needle);
    if (p == std::string::npos) return "";
    size_t e = qs.find('&', p);
    return qs.substr(p + needle.size(), e == std::string::npos ? std::string::npos : e - p - needle.size());
}

static void serve_client(int fd) {
    set_nodelay(fd);
    std::string buf;
    for (;;) {
        ParseResult pr;
        for (;;) {
            pr = parse_http_request(buf, 65536, 512u * 1024 * 1024);
            if (pr.status != ParseStatus::NeedMore) break;
            char tmp[16384];
            ssize_t n = recv_some(fd, tmp, sizeof(tmp), 30000);
            if (n <= 0) goto done;
            buf.append(tmp, static_cast<size_t>(n));
        }
        if (pr.status == ParseStatus::Bad) {
            send_all(fd, build_response(pr.error_status, "text/plain", pr.error + "\n", false), 5000);
            break;
        }
        buf.erase(0, pr.consumed);
        {
            const HttpRequest& req = pr.req;
            std::string path = req.target.substr(0, req.target.find('?'));
            const bool head = (req.method == "HEAD");
            bool keep = req.keep_alive;
            std::string out;

            if (path == "/health") {
                out = build_response(200, "application/json", "{\"status\":\"ok\"}\n", keep, head);
            } else if (path == "/slow") {
                uint64_t ms = 0;
                parse_uint(query_value(req.target, "ms"), ms, 6);
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                out = build_response(200, "application/json", "{\"status\":\"ok\",\"slept_ms\":" + std::to_string(ms) + "}\n", keep, head);
            } else if (path.rfind("/status/", 0) == 0) {
                uint64_t code = 200;
                parse_uint(path.substr(8), code, 3);
                if (code < 200 || code > 599 || code == 204 || code == 304) code = 200;
                std::string body = "status " + std::to_string(code) + "\n";
                out = "HTTP/1.1 " + std::to_string(code) + " X\r\nContent-Type: text/plain\r\nContent-Length: " +
                      std::to_string(body.size()) + (keep ? "\r\nConnection: keep-alive\r\n\r\n" : "\r\nConnection: close\r\n\r\n") +
                      (head ? "" : body);
            } else if (path == "/chunked") {
                std::string body = "5\r\nhello\r\n7\r\n world!\r\n0\r\n\r\n";
                out = std::string("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nConnection: ") +
                      (keep ? "keep-alive" : "close") + "\r\n\r\n" + (head ? "" : body);
            } else if (path == "/close") {
                out = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nunframed-body";
                keep = false;
            } else if (path == "/big") {
                uint64_t n = 1024;
                parse_uint(query_value(req.target, "bytes"), n, 9);
                out = build_response(200, "application/octet-stream", std::string(static_cast<size_t>(n), 'x'), keep, head);
            } else {
                std::string body = "{\"status\":\"ok\",\"method\":\"" + json_escape(req.method) + "\",\"path\":\"" +
                                   json_escape(req.target) + "\",\"body_bytes\":" + std::to_string(req.body.size()) + "}\n";
                out = build_response(200, "application/json", body, keep, head);
            }
            if (!send_all(fd, out, 30000) || !keep) break;
        }
    }
done:
    ::close(fd);
    g_connections.fetch_sub(1);
}

int main(int argc, char* argv[]) {
    uint16_t port = 9000;
    std::string listen_addr = "0.0.0.0";   // "::" = IPv6 + IPv4 (dual stack)
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--port") {
            try { port = static_cast<uint16_t>(std::stoi(argv[i + 1])); } catch (...) { std::cerr << "invalid --port\n"; return 1; }
        } else if (a == "--listen") {
            listen_addr = argv[i + 1];
        } else {
            std::cerr << "usage: target_server [--port N] [--listen ADDR]\n";
            return 1;
        }
    }
    std::signal(SIGPIPE, SIG_IGN);
    int lfd = listen_on(listen_addr, port, 4096);
    if (lfd < 0) {
        std::perror("target_server bind/listen");
        return 1;
    }
    std::cout << "[TargetServer] Listening on port " << port << std::endl;
    for (;;) {
        int cfd = ::accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) continue;
        if (g_connections.fetch_add(1) >= 8192) { ::close(cfd); g_connections.fetch_sub(1); continue; }
        std::thread(serve_client, cfd).detach();
    }
}
