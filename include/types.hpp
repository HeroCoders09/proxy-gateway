#pragma once

#include "http.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <netinet/in.h>
#include <unistd.h>

namespace gateway {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class RoutingPolicy {
    RANDOM,
    ROUND_ROBIN,
    LEAST_CONNECTIONS,
    LOWEST_LATENCY,
    WEIGHTED
};

enum class CircuitState {
    CLOSED,
    OPEN,
    HALF_OPEN
};

enum class ConnState {
    IDLE,
    CONNECTING,
    ACTIVE,
    CLOSED
};

inline std::string policy_to_string(RoutingPolicy p) {
    switch (p) {
        case RoutingPolicy::RANDOM: return "random";
        case RoutingPolicy::ROUND_ROBIN: return "round_robin";
        case RoutingPolicy::LEAST_CONNECTIONS: return "least_connections";
        case RoutingPolicy::LOWEST_LATENCY: return "lowest_latency";
        case RoutingPolicy::WEIGHTED: return "weighted";
    }
    return "weighted";
}

inline std::string circuit_to_string(CircuitState s) {
    switch (s) {
        case CircuitState::CLOSED: return "CLOSED";
        case CircuitState::OPEN: return "OPEN";
        case CircuitState::HALF_OPEN: return "HALF-OPEN";
    }
    return "CLOSED";
}

struct RateLimitConfig {
    double rate = 200.0;
    double burst = 400.0;
};

// A resolved socket address of either family (IPv4 or IPv6).
struct SockAddr {
    sockaddr_storage ss{};
    socklen_t len = 0;
    int family() const { return ss.ss_family; }
    const sockaddr* sa() const { return reinterpret_cast<const sockaddr*>(&ss); }
};

enum class ProxyProtocol { HTTP, SOCKS5 };

inline const char* protocol_name(ProxyProtocol p) { return p == ProxyProtocol::SOCKS5 ? "socks5" : "http"; }

struct ProxyEndpointConfig {
    std::string host;
    uint16_t port = 0;
    ProxyProtocol protocol = ProxyProtocol::HTTP;
    std::string username{}, password{};   // SOCKS5 username/password (RFC 1929); both empty = no authentication
};

struct GatewayConfig {
    std::string listen_host = "127.0.0.1";   // loopback by default: the forward proxy has no client authentication
    uint16_t listen_port = 8000;
    size_t max_queue_size = 10000;
    int max_retries = 2;
    RoutingPolicy policy = RoutingPolicy::WEIGHTED;

    double w_latency = 0.5;
    double w_load = 0.3;
    double w_failure = 0.2;

    int workers_min = 4;
    int workers_max = 64;
    int workers_initial = 16;
    int aimd_alpha = 2;
    double aimd_beta = 0.5;

    int connect_timeout_ms = 2000;
    int response_timeout_ms = 10000;
    int probe_interval_ms = 2000;
    int io_threads = 0;                // event-loop threads for upstream I/O (0 = auto: min(4, cores))
    int queue_timeout_ms = 30000;      // max time a request may wait in a domain queue (0 = unlimited)
    int client_idle_timeout_ms = 30000; // idle keep-alive client connections are closed after this
    int client_header_timeout_ms = 10000; // time allowed to deliver a complete request once started
    int tunnel_idle_timeout_ms = 300000;  // CONNECT tunnels idle this long are closed
    int upstream_idle_timeout_ms = 30000; // pooled keep-alive connections to upstream proxies are closed after this much idle time

    size_t max_header_bytes = 64 * 1024;
    size_t max_body_bytes = 10 * 1024 * 1024;
    size_t max_response_bytes = 64 * 1024 * 1024;
    size_t max_client_connections = 10000;
    size_t pool_max_idle_per_proxy = 64;     // idle keep-alive upstream connections kept per pool key
    size_t pool_max_idle_total = 1024;       // ... and overall (each idle connection holds a file descriptor)

    int circuit_threshold = 5;
    int circuit_cooldown_sec = 10;

    RateLimitConfig default_rate_limit{200.0, 400.0};
    std::unordered_map<std::string, RateLimitConfig> domain_rate_limits;
    std::vector<ProxyEndpointConfig> proxies;
    std::string admin_token;          // non-empty: /metrics and /proxies require "Authorization: Bearer <token>"
    uint16_t admin_port = 0;          // >0: admin endpoints live ONLY on this dedicated listener (default: shared proxy port)
    std::string admin_host = "127.0.0.1";   // address of the dedicated admin listener
    bool admin_allow_remote = false;  // shared port only: also serve admin endpoints to non-local clients (needs a token)
    bool inline_secrets = false;      // a password/token is written directly in the config file
    std::string probe_target_url = "http://127.0.0.1:9000/health";

    std::string csv_output_path = "results.csv";
    std::string json_output_path = "results.jsonl";

    bool check_only = false;  // --check-config: validate and exit
};

// A client TCP connection. It is owned either by the event loop (waiting for the next request,
// registered in epoll) or by exactly one job/worker (in_flight == true).
struct ClientConn {
    int fd = -1;
    std::string buf;                 // bytes received but not yet consumed
    bool sent_continue = false;
    bool peer_eof = false;
    bool admin_only = false;         // accepted on the dedicated admin listener: serves admin paths only
    bool peer_loopback = false;      // peer address is a loopback address
    // Ownership handoff: the thread that finishes with a connection `arm()`s it (release), the event loop that
    // next receives its epoll event acquires. Makes the plain fields (buf, peer_eof, ...) safely transferable.
    std::atomic<uint32_t> handoff{0};
    std::atomic<bool> in_flight{false};
    std::atomic<bool> partial{false};  // armed with an incomplete request in `buf` (header timeout applies)
    std::atomic<int64_t> last_activity_ms{0};

    explicit ClientConn(int f) : fd(f) { touch(); }
    ~ClientConn() { if (fd >= 0) ::close(fd); }
    ClientConn(const ClientConn&) = delete;
    ClientConn& operator=(const ClientConn&) = delete;

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    }
    void touch() { last_activity_ms.store(now_ms(), std::memory_order_relaxed); }
    int64_t idle_ms() const { return now_ms() - last_activity_ms.load(std::memory_order_relaxed); }
};

struct Job {
    uint64_t id = 0;
    std::shared_ptr<ClientConn> client;
    std::string method;
    std::string url;       // absolute URL (or "host:port" for CONNECT)
    std::string domain;    // scheduling key (lower-cased authority)
    std::string path;
    uint16_t target_port = 80;
    HeaderList headers;
    std::string body;
    bool keep_alive = true;
    TimePoint arrival_time = Clock::now();
    TimePoint deadline = TimePoint::max();  // queue deadline
    int retry_count = 0;
};

struct ProxyState {
    int id = 0;
    std::string host;
    uint16_t port = 0;
    ProxyProtocol protocol = ProxyProtocol::HTTP;
    std::string username, password;   // never logged or exposed by /proxies

    mutable std::mutex mtx;
    bool healthy = true;
    double rtt_ms = 10.0;
    std::deque<double> recent_rtts; // Rolling window of last N RTTs
    uint64_t success_count = 0;
    uint64_t failure_count = 0;
    int consecutive_failures = 0;
    std::atomic<int> active_connections{0};
    CircuitState circuit = CircuitState::CLOSED;
    TimePoint circuit_opened_at = Clock::now();
    TimePoint last_probe_time = Clock::now();
    bool half_open_probe_in_flight = false;
    SockAddr addr;           // first resolved address (refreshed by the prober thread, never on the I/O path)
    std::vector<SockAddr> addrs;   // ALL resolved addresses (e.g. ::1 and 127.0.0.1): connect fails over across them
    mutable std::atomic<size_t> addr_pref{0};   // index of the address that connected last (tried first)
    bool addr_valid = false;
    mutable std::string last_error;   // why the latest health probe failed ("" when it succeeded); guarded by mtx

    ProxyState(int i, std::string h, uint16_t p, ProxyProtocol proto = ProxyProtocol::HTTP)
        : id(i), host(std::move(h)), port(p), protocol(proto) {}

    std::string endpoint() const {
        return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    }
};

// RAII Socket wrapper for connection pool
struct Connection {
    int fd = -1;
    int proxy_id = -1;
    std::string key;                 // pool key: proxy id (HTTP) or proxy id + destination (SOCKS5 tunnels)
    ConnState state = ConnState::CLOSED;
    TimePoint last_used = Clock::now();

    Connection() = default;
    Connection(int sock_fd, int pid, ConnState st)
        : fd(sock_fd), proxy_id(pid), state(st), last_used(Clock::now()) {}

    ~Connection() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept
        : fd(other.fd), proxy_id(other.proxy_id), state(other.state), last_used(other.last_used) {
        other.fd = -1;
        other.state = ConnState::CLOSED;
    }

    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) ::close(fd);
            fd = other.fd;
            proxy_id = other.proxy_id;
            state = other.state;
            last_used = other.last_used;
            other.fd = -1;
            other.state = ConnState::CLOSED;
        }
        return *this;
    }
};

} // namespace gateway
