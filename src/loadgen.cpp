// Closed-loop HTTP load generator. Each connection (one thread) sends requests through a proxy
// (absolute-URI form) over a persistent connection and records latency.
//
//   loadgen --proxy-port 8000 --url http://127.0.0.1:9000/products/1 --conns 128 --duration 10
//           [--proxy-host 127.0.0.1] [--method GET|POST] [--body-bytes N] [--no-keepalive]
#include "http.hpp"
#include "net.hpp"
#include <algorithm>
#include <atomic>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

using namespace gateway;

struct Args {
    std::string proxy_host = "127.0.0.1";
    uint16_t proxy_port = 8000;
    std::string url = "http://127.0.0.1:9000/products/1";
    std::string method = "GET";
    size_t body_bytes = 0;
    int conns = 64;
    int duration = 10;
    bool keepalive = true;
};

struct Stats {
    std::vector<uint32_t> lat_us;
    std::map<int, uint64_t> by_status;
    uint64_t errors = 0;
    uint64_t reconnects = 0;
};

static void worker(const Args& a, Stats& st, NetClock::time_point stop_at) {
    UrlParts u;
    parse_absolute_url(a.url, u);
    std::string body(a.body_bytes, 'b');
    std::string req = a.method + " " + a.url + " HTTP/1.1\r\nHost: " + u.authority + "\r\n";
    if (a.body_bytes > 0 || a.method == "POST") req += "Content-Length: " + std::to_string(a.body_bytes) + "\r\n";
    req += a.keepalive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    req += body;

    int fd = -1;
    while (NetClock::now() < stop_at) {
        if (fd < 0) {
            bool to = false;
            fd = connect_with_timeout(a.proxy_host, a.proxy_port, 3000, to);
            if (fd < 0) { st.errors++; std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
            st.reconnects++;
        }
        auto t0 = NetClock::now();
        ResponseData rd;
        bool ok = send_all(fd, req, 10000) &&
                  read_http_response(fd, a.method == "HEAD", 15000, 64u * 1024 * 1024, rd) == ReadStatus::Ok;
        if (!ok) {
            st.errors++;
            ::close(fd);
            fd = -1;
            continue;
        }
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(NetClock::now() - t0).count();
        st.lat_us.push_back(static_cast<uint32_t>(std::min<long long>(us, 4000000000LL)));
        st.by_status[rd.status]++;
        if (!a.keepalive || !rd.framed || rd.close_header) { ::close(fd); fd = -1; }
    }
    if (fd >= 0) ::close(fd);
}

int main(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::cerr << "missing value for " << k << "\n"; std::exit(1); } return argv[++i]; };
        try {
            if (k == "--proxy-host") a.proxy_host = next();
            else if (k == "--proxy-port") a.proxy_port = static_cast<uint16_t>(std::stoi(next()));
            else if (k == "--url") a.url = next();
            else if (k == "--method") a.method = next();
            else if (k == "--body-bytes") a.body_bytes = static_cast<size_t>(std::stoul(next()));
            else if (k == "--conns") a.conns = std::stoi(next());
            else if (k == "--duration") a.duration = std::stoi(next());
            else if (k == "--no-keepalive") a.keepalive = false;
            else { std::cerr << "unknown option " << k << "\n"; return 1; }
        } catch (const std::exception&) { std::cerr << "invalid value for " << k << "\n"; return 1; }
    }
    UrlParts probe;
    if (!parse_absolute_url(a.url, probe) || a.conns < 1 || a.duration < 1) { std::cerr << "invalid arguments\n"; return 1; }
    std::signal(SIGPIPE, SIG_IGN);

    std::vector<Stats> stats(static_cast<size_t>(a.conns));
    std::vector<std::thread> threads;
    auto start = NetClock::now();
    auto stop_at = start + std::chrono::seconds(a.duration);
    for (int i = 0; i < a.conns; ++i) threads.emplace_back(worker, std::cref(a), std::ref(stats[static_cast<size_t>(i)]), stop_at);
    for (auto& t : threads) t.join();
    double secs = std::chrono::duration<double>(NetClock::now() - start).count();

    std::vector<uint32_t> all;
    std::map<int, uint64_t> by_status;
    uint64_t errors = 0, reconnects = 0;
    for (auto& s : stats) {
        all.insert(all.end(), s.lat_us.begin(), s.lat_us.end());
        for (auto& kv : s.by_status) by_status[kv.first] += kv.second;
        errors += s.errors;
        reconnects += s.reconnects;
    }
    std::sort(all.begin(), all.end());
    auto pct = [&](double p) { return all.empty() ? 0.0 : all[static_cast<size_t>(p * static_cast<double>(all.size() - 1))] / 1000.0; };
    uint64_t total = all.size(), ok = by_status.count(200) ? by_status[200] : 0;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "requests=" << total << " ok200=" << ok << " errors=" << errors << " connections_opened=" << reconnects << "\n";
    std::cout << "duration_s=" << secs << " rps=" << (secs > 0 ? total / secs : 0) << "\n";
    std::cout << "latency_ms p50=" << pct(0.50) << " p95=" << pct(0.95) << " p99=" << pct(0.99)
              << " max=" << (all.empty() ? 0.0 : all.back() / 1000.0) << "\n";
    std::cout << "status:";
    for (auto& kv : by_status) std::cout << " " << kv.first << "x" << kv.second;
    std::cout << "\n";
    return (total == 0) ? 2 : 0;
}
