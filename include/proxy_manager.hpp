#pragma once

#include "net.hpp"
#include "types.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace gateway {

class ProxyManager {
public:
    explicit ProxyManager(const GatewayConfig& cfg)
        : cfg_(cfg), rng_(std::random_device{}()) {
        int id = 0;
        for (const auto& ep : cfg.proxies) {
            proxies_.push_back(std::make_shared<ProxyState>(id++, ep.host, ep.port, ep.protocol));
            proxies_.back()->username = ep.username;
            proxies_.back()->password = ep.password;
            refresh_address(*proxies_.back());  // startup only: may block on DNS
        }
    }

    ~ProxyManager() {
        stop_prober();
    }

    void start_prober() {
        running_.store(true);
        probe_thread_ = std::thread([this]() { probe_loop(); });
    }

    void stop_prober() {
        if (running_.exchange(false)) {
            {
                std::lock_guard<std::mutex> lk(probe_mtx_);
            }
            probe_cv_.notify_all();
            if (probe_thread_.joinable()) {
                probe_thread_.join();
            }
        }
    }

    // Select an eligible proxy excluding any proxies already tried for this job
    std::shared_ptr<ProxyState> select_proxy(const std::vector<int>& excluded_ids = {}) {
        std::vector<std::shared_ptr<ProxyState>> eligible;
        TimePoint now = Clock::now();

        for (auto& p : proxies_) {
            if (std::find(excluded_ids.begin(), excluded_ids.end(), p->id) != excluded_ids.end()) {
                continue;
            }
            std::lock_guard<std::mutex> lock(p->mtx);
            if (p->circuit == CircuitState::OPEN) {
                double elapsed_sec = std::chrono::duration<double>(now - p->circuit_opened_at).count();
                if (elapsed_sec >= cfg_.circuit_cooldown_sec) {
                    p->circuit = CircuitState::HALF_OPEN;
                    p->half_open_probe_in_flight = false;
                } else {
                    continue;
                }
            }
            if (p->circuit == CircuitState::HALF_OPEN) {
                if (p->half_open_probe_in_flight) {
                    continue;
                }
                p->half_open_probe_in_flight = true;
                p->active_connections.fetch_add(1, std::memory_order_relaxed);
                return p; // Route recovery test request
            }
            if (!p->healthy) {
                continue;
            }
            eligible.push_back(p);
        }

        if (eligible.empty()) {
            return nullptr;
        }

        std::shared_ptr<ProxyState> chosen = apply_routing_policy(eligible);
        if (chosen) {
            chosen->active_connections.fetch_add(1, std::memory_order_relaxed);
        }
        return chosen;
    }

    void record_outcome(int proxy_id, bool success, double rtt_ms) {
        if (proxy_id < 0 || proxy_id >= static_cast<int>(proxies_.size())) return;
        auto& p = proxies_[proxy_id];
        p->active_connections.fetch_sub(1, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(p->mtx);
        update_proxy_state_locked(*p, success, rtt_ms);
    }

    // Resolved address of a proxy for the non-blocking I/O path (no DNS here). False if unresolved.
    static bool address_of(const ProxyState& p, SockAddr& out) {
        std::lock_guard<std::mutex> lock(p.mtx);
        if (!p.addr_valid) return false;
        out = p.addr;
        return true;
    }

    // All resolved addresses, the one that worked last first (for connect fail-over). False if unresolved.
    static bool address_list_of(const ProxyState& p, std::vector<SockAddr>& out) {
        std::lock_guard<std::mutex> lock(p.mtx);
        if (!p.addr_valid || p.addrs.empty()) return false;
        out = p.addrs;
        size_t pref = p.addr_pref.load(std::memory_order_relaxed);
        if (pref > 0 && pref < out.size()) std::rotate(out.begin(), out.begin() + static_cast<long>(pref), out.end());
        return true;
    }

    const std::vector<std::shared_ptr<ProxyState>>& proxies() const {
        return proxies_;
    }

    std::string render_proxies_json() const {
        std::ostringstream ss;
        ss << std::fixed;
        ss.precision(2);
        ss << "[";
        for (size_t i = 0; i < proxies_.size(); ++i) {
            const auto& p = proxies_[i];
            std::lock_guard<std::mutex> lock(p->mtx);
            if (i > 0) ss << ",";
            ss << "{\"id\":" << p->id
               << ",\"endpoint\":\"" << json_escape(p->endpoint()) << "\""
               << ",\"protocol\":\"" << protocol_name(p->protocol) << "\""
               << ",\"healthy\":" << (p->healthy ? "true" : "false")
               << ",\"rtt_ms\":" << p->rtt_ms
               << ",\"active_connections\":" << p->active_connections.load()
               << ",\"success_count\":" << p->success_count
               << ",\"failure_count\":" << p->failure_count
               << ",\"consecutive_failures\":" << p->consecutive_failures
               << ",\"circuit_state\":\"" << circuit_to_string(p->circuit) << "\""
               << ",\"last_error\":\"" << json_escape(p->last_error) << "\"}";
        }
        ss << "]";
        return ss.str();
    }

private:
    std::shared_ptr<ProxyState> apply_routing_policy(
        const std::vector<std::shared_ptr<ProxyState>>& eligible) {
        if (eligible.size() == 1) return eligible.front();

        switch (cfg_.policy) {
            case RoutingPolicy::RANDOM: {
                std::lock_guard<std::mutex> lock(policy_mtx_);
                std::uniform_int_distribution<size_t> dist(0, eligible.size() - 1);
                return eligible[dist(rng_)];
            }
            case RoutingPolicy::ROUND_ROBIN: {
                size_t idx = rr_counter_.fetch_add(1, std::memory_order_relaxed) % eligible.size();
                return eligible[idx];
            }
            case RoutingPolicy::LEAST_CONNECTIONS: {
                return *std::min_element(eligible.begin(), eligible.end(),
                    [](const auto& a, const auto& b) {
                        return a->active_connections.load() < b->active_connections.load();
                    });
            }
            case RoutingPolicy::LOWEST_LATENCY: {
                // Snapshot RTTs one lock at a time (never hold two proxy locks at once).
                std::shared_ptr<ProxyState> best = eligible.front();
                double best_rtt = 1e18;
                for (const auto& p : eligible) {
                    double rtt;
                    {
                        std::lock_guard<std::mutex> lock(p->mtx);
                        rtt = p->rtt_ms;
                    }
                    if (rtt < best_rtt) { best_rtt = rtt; best = p; }
                }
                return best;
            }
            case RoutingPolicy::WEIGHTED: {
                // S_i = w_L * L_i + w_C * C_i + w_F * F_i (normalized across eligible set)
                double max_rtt = 1.0;
                double max_conn = 1.0;
                struct Snap {
                    std::shared_ptr<ProxyState> proxy;
                    double rtt;
                    double conn;
                    double fail_rate;
                };
                std::vector<Snap> snaps;
                snaps.reserve(eligible.size());
                for (const auto& p : eligible) {
                    std::lock_guard<std::mutex> lock(p->mtx);
                    double c = static_cast<double>(p->active_connections.load());
                    uint64_t tot = p->success_count + p->failure_count;
                    double fr = (tot > 0) ? (static_cast<double>(p->failure_count) / tot) : 0.0;
                    max_rtt = std::max(max_rtt, p->rtt_ms);
                    max_conn = std::max(max_conn, c);
                    snaps.push_back({p, p->rtt_ms, c, fr});
                }
                std::shared_ptr<ProxyState> best = snaps.front().proxy;
                double best_score = 1e18;
                for (const auto& s : snaps) {
                    double norm_l = s.rtt / max_rtt;
                    double norm_c = s.conn / max_conn;
                    double norm_f = s.fail_rate;
                    double score = cfg_.w_latency * norm_l +
                                   cfg_.w_load * norm_c +
                                   cfg_.w_failure * norm_f;
                    if (score < best_score) {
                        best_score = score;
                        best = s.proxy;
                    }
                }
                return best;
            }
        }
        return eligible.front();
    }

    void update_proxy_state_locked(ProxyState& p, bool success, double rtt_ms) {
        if (success) {
            p.success_count++;
            p.consecutive_failures = 0;
            p.healthy = true;
            p.half_open_probe_in_flight = false;
            p.circuit = CircuitState::CLOSED;

            if (p.recent_rtts.size() >= 10) {
                p.recent_rtts.pop_front();
            }
            p.recent_rtts.push_back(rtt_ms);
            double sum = 0.0;
            for (double v : p.recent_rtts) sum += v;
            p.rtt_ms = sum / static_cast<double>(p.recent_rtts.size());
        } else {
            p.failure_count++;
            p.consecutive_failures++;
            p.half_open_probe_in_flight = false;

            if (p.circuit == CircuitState::HALF_OPEN ||
                p.consecutive_failures >= cfg_.circuit_threshold) {
                p.circuit = CircuitState::OPEN;
                p.circuit_opened_at = Clock::now();
                p.healthy = false;
            }
        }
    }

    // (Re-)resolve a proxy's host name. Called at startup and from the prober thread only.
    static void refresh_address(ProxyState& p) {
        std::vector<SockAddr> all;
        if (!resolve_all(p.host, p.port, all)) return;  // keep the last known addresses
        std::lock_guard<std::mutex> lock(p.mtx);
        if (p.addrs.empty() || p.addrs.size() != all.size()) p.addr_pref.store(0, std::memory_order_relaxed);
        p.addr = all.front();
        p.addrs = std::move(all);
        p.addr_valid = true;
    }

    // FR-7: Periodic health probe every probe_interval_ms (default 2 s)
    void probe_loop() {
        while (running_.load()) {
            for (auto& p : proxies_) {
                if (!running_.load()) break;
                refresh_address(*p);
                double rtt = 0.0;
                bool ok = execute_single_probe(*p, rtt);
                std::lock_guard<std::mutex> lock(p->mtx);
                p->last_probe_time = Clock::now();
                if (p->circuit == CircuitState::OPEN) {
                    double elapsed = std::chrono::duration<double>(
                                         Clock::now() - p->circuit_opened_at).count();
                    if (elapsed >= cfg_.circuit_cooldown_sec) {
                        p->circuit = CircuitState::HALF_OPEN;
                    }
                }
                if (p->circuit != CircuitState::OPEN) {
                    update_proxy_state_locked(*p, ok, rtt);
                } else if (ok) {
                    // Cooldown expired or background recovery verified
                    double elapsed = std::chrono::duration<double>(
                                         Clock::now() - p->circuit_opened_at).count();
                    if (elapsed >= cfg_.circuit_cooldown_sec) {
                        update_proxy_state_locked(*p, true, rtt);
                    }
                }
            }
            std::unique_lock<std::mutex> lk(probe_mtx_);
            probe_cv_.wait_for(lk, std::chrono::milliseconds(cfg_.probe_interval_ms),
                               [this]() { return !running_.load(); });
        }
    }

    static void set_probe_error(const ProxyState& p, const std::string& why) {
        std::lock_guard<std::mutex> lock(p.mtx);
        p.last_error = why;
    }

    bool execute_single_probe(const ProxyState& p, double& rtt_ms_out) {
        auto t0 = Clock::now();
        std::vector<SockAddr> addrs;
        if (!address_list_of(p, addrs)) { set_probe_error(p, "cannot resolve the proxy host name"); return false; }
        bool timed_out = false;
        size_t used = 0;
        int fd = connect_addrs(addrs, cfg_.connect_timeout_ms, timed_out, &used);
        if (fd < 0) {
            set_probe_error(p, timed_out ? "cannot connect to the proxy: timed out (dead, firewalled or wrong IP/port)"
                                         : "cannot connect to the proxy: connection refused or unreachable (wrong IP/port, or it is down)");
            return false;
        }
        p.addr_pref.store((used + p.addr_pref.load(std::memory_order_relaxed)) % addrs.size(), std::memory_order_relaxed);

        // The Host header must name the probe target: real proxies and CDN-fronted sites reject (or misroute) a made-up Host.
        UrlParts u;
        const bool url_ok = parse_absolute_url(cfg_.probe_target_url, u);
        std::string req = "GET " + cfg_.probe_target_url + " HTTP/1.1\r\nHost: " + (url_ok ? u.authority : std::string("health.probe")) +
                          "\r\nUser-Agent: proxy-gateway-probe/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n";
        if (p.protocol == ProxyProtocol::SOCKS5) {   // tunnel to the probe target, then speak plain origin-form HTTP
            std::string why;
            if (!url_ok) { ::close(fd); set_probe_error(p, "invalid probe_target_url (must be http://host/path)"); return false; }
            if (!socks5_handshake_blocking(fd, u.host, u.port, cfg_.connect_timeout_ms, p.username, p.password, &why)) {
                ::close(fd);
                set_probe_error(p, why.empty() ? "SOCKS5 handshake failed" : why);
                return false;
            }
            req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.authority + "\r\nConnection: close\r\n\r\n";
        }
        ssize_t sent = ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
        if (sent <= 0) {
            ::close(fd);
            set_probe_error(p, "proxy closed the connection while receiving the probe request");
            return false;
        }

        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, cfg_.connect_timeout_ms) <= 0) {
            ::close(fd);
            set_probe_error(p, "proxy accepted the connection but sent no answer in time (too slow, or the probe URL is unreachable through it)");
            return false;
        }
        char buf[512];
        ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        ::close(fd);
        if (n <= 0) {
            set_probe_error(p, "proxy closed the connection without answering (it may not allow this site/port, or speak another protocol)");
            return false;
        }
        buf[n] = '\0';

        rtt_ms_out = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        const char* eol = static_cast<const char*>(std::memchr(buf, '\r', static_cast<size_t>(n)));
        int status = 0;
        if (!parse_status_line(buf, eol ? static_cast<size_t>(eol - buf) : static_cast<size_t>(n), status)) {
            set_probe_error(p, p.protocol == ProxyProtocol::SOCKS5 ? "the target did not answer with HTTP through the SOCKS5 proxy"
                                                                  : "the proxy did not answer with HTTP (wrong protocol? is it really an HTTP proxy?)");
            return false;
        }
        if (status < 200 || status >= 300) {
            set_probe_error(p, "probe answered HTTP " + std::to_string(status) +
                               (status == 407 ? " (proxy authentication required)" : status == 403 ? " (blocked by the proxy or the site)" :
                                status >= 300 && status < 400 ? " (redirect: use a probe URL that answers 200 directly)" : ""));
            return false;
        }
        set_probe_error(p, "");
        return true;
    }

    GatewayConfig cfg_;
    std::vector<std::shared_ptr<ProxyState>> proxies_;
    std::atomic<size_t> rr_counter_{0};
    std::mutex policy_mtx_;
    std::mt19937 rng_;
    std::atomic<bool> running_{false};
    std::thread probe_thread_;
    std::mutex probe_mtx_;
    std::condition_variable probe_cv_;
};

} // namespace gateway
