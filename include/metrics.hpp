#pragma once
#include <dirent.h>
#include <sys/resource.h>

#include "types.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace gateway {

struct LatencyPercentiles {
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
};

class MetricsManager {
public:
    std::atomic<uint64_t> requests_total{0};
    std::atomic<uint64_t> requests_successful{0};
    std::atomic<uint64_t> requests_failed{0};
    std::atomic<uint64_t> timeouts{0};
    std::atomic<uint64_t> queue_timeouts{0};
    std::atomic<uint64_t> tunnels_total{0};
    std::atomic<int> active_tunnels{0};
    std::atomic<int> active_connections{0};
    std::atomic<int> client_connections{0};
    std::atomic<size_t> pool_idle{0};          // idle pooled upstream connections (set by the server before rendering)
    std::atomic<int> worker_limit{0};
    std::atomic<int> active_workers{0};
    std::atomic<size_t> queue_length{0};

    explicit MetricsManager(const GatewayConfig& cfg)
        : start_time_(Clock::now()), last_flush_(Clock::now()) {
        worker_limit.store(cfg.workers_initial);
        if (!cfg.csv_output_path.empty()) {
            csv_.open(cfg.csv_output_path, std::ios::out | std::ios::trunc);
            if (csv_.is_open()) csv_ << "timestamp_ms,url,status,proxy,latency_ms,success\n";
        }
        if (!cfg.json_output_path.empty()) {
            json_.open(cfg.json_output_path, std::ios::out | std::ios::trunc);
        }
    }

    ~MetricsManager() { flush(); }

    void flush() {
        std::lock_guard<std::mutex> io_lock(file_mtx_);
        if (csv_.is_open()) csv_.flush();
        if (json_.is_open()) json_.flush();
    }

    void record_completion(const std::string& url, int status, const std::string& proxy_ep,
                           double latency_ms, bool success, bool is_timeout) {
        requests_total.fetch_add(1, std::memory_order_relaxed);
        if (success) requests_successful.fetch_add(1, std::memory_order_relaxed);
        else requests_failed.fetch_add(1, std::memory_order_relaxed);
        if (is_timeout) timeouts.fetch_add(1, std::memory_order_relaxed);

        auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
        {
            std::lock_guard<std::mutex> lock(lat_mtx_);
            if (latency_samples_.size() >= 10000) latency_samples_.pop_front();
            latency_samples_.push_back(latency_ms);
        }

        if (!csv_.is_open() && !json_.is_open()) return;
        std::ostringstream num;
        num << std::fixed << std::setprecision(2) << latency_ms;
        std::lock_guard<std::mutex> io_lock(file_mtx_);
        if (csv_.is_open()) {
            csv_ << ts_ms << ',' << csv_escape(url) << ',' << status << ',' << csv_escape(proxy_ep) << ','
                 << num.str() << ',' << (success ? 1 : 0) << '\n';
        }
        if (json_.is_open()) {
            json_ << "{\"timestamp_ms\":" << ts_ms << ",\"url\":\"" << json_escape(url) << "\",\"status\":" << status
                  << ",\"proxy\":\"" << json_escape(proxy_ep) << "\",\"latency_ms\":" << num.str()
                  << ",\"success\":" << (success ? "true" : "false") << "}\n";
        }
        auto now = Clock::now();
        if (now - last_flush_ > std::chrono::milliseconds(200)) {
            if (csv_.is_open()) csv_.flush();
            if (json_.is_open()) json_.flush();
            last_flush_ = now;
        }
    }

    LatencyPercentiles compute_percentiles() const {
        std::vector<double> copy;
        {
            std::lock_guard<std::mutex> lock(lat_mtx_);
            copy.assign(latency_samples_.begin(), latency_samples_.end());
        }
        if (copy.empty()) return {};
        std::sort(copy.begin(), copy.end());
        auto at_pct = [&](double p) {
            size_t idx = static_cast<size_t>(p * static_cast<double>(copy.size() - 1));
            return copy[idx];
        };
        return {at_pct(0.50), at_pct(0.95), at_pct(0.99)};
    }

    double current_rps() const {
        double elapsed = std::chrono::duration<double>(Clock::now() - start_time_).count();
        if (elapsed <= 0.001) return 0.0;
        return static_cast<double>(requests_total.load()) / elapsed;
    }

    // Prometheus label values: backslash, double quote and newline must be escaped.
    static std::string prom_label(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '\\' || c == '"') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else if (static_cast<unsigned char>(c) >= 0x20) o += c;
        }
        return o;
    }

    static long count_open_fds() {
        long n = 0;
        if (DIR* d = ::opendir("/proc/self/fd")) {
            while (::readdir(d)) ++n;
            ::closedir(d);
            n -= 3;   // ".", ".." and the directory stream's own descriptor
        }
        return n < 0 ? 0 : n;
    }
    static long fd_limit() {
        rlimit rl{};
        return ::getrlimit(RLIMIT_NOFILE, &rl) == 0 ? static_cast<long>(rl.rlim_cur) : -1;
    }

    std::string render_prometheus(const std::vector<std::shared_ptr<ProxyState>>& proxies) const {
        auto pct = compute_percentiles();
        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        out << "# HELP requests_total Total requests processed by the gateway\n"
            << "# TYPE requests_total counter\n"
            << "requests_total " << requests_total.load() << "\n"
            << "# HELP requests_successful Total successful requests\n"
            << "# TYPE requests_successful counter\n"
            << "requests_successful " << requests_successful.load() << "\n"
            << "# HELP requests_failed Total failed requests\n"
            << "# TYPE requests_failed counter\n"
            << "requests_failed " << requests_failed.load() << "\n"
            << "# HELP timeouts Total upstream timeouts\n"
            << "# TYPE timeouts counter\n"
            << "timeouts " << timeouts.load() << "\n"
            << "# HELP queue_timeouts Requests dropped after waiting too long in a domain queue\n"
            << "# TYPE queue_timeouts counter\n"
            << "queue_timeouts " << queue_timeouts.load() << "\n"
            << "# HELP tunnels_total CONNECT tunnels established\n"
            << "# TYPE tunnels_total counter\n"
            << "tunnels_total " << tunnels_total.load() << "\n"
            << "# HELP active_tunnels Currently open CONNECT tunnels\n"
            << "# TYPE active_tunnels gauge\n"
            << "active_tunnels " << active_tunnels.load() << "\n"
            << "# HELP upstream_idle_connections Idle keep-alive upstream connections held in the pool\n"
            << "# TYPE upstream_idle_connections gauge\n"
            << "upstream_idle_connections " << pool_idle.load() << "\n"
            << "# HELP process_open_fds Open file descriptors of the gateway process\n"
            << "# TYPE process_open_fds gauge\n"
            << "process_open_fds " << count_open_fds() << "\n"
            << "# HELP process_fd_limit Soft RLIMIT_NOFILE of the gateway process\n"
            << "# TYPE process_fd_limit gauge\n"
            << "process_fd_limit " << fd_limit() << "\n"
            << "# HELP client_connections Currently open client connections\n"
            << "# TYPE client_connections gauge\n"
            << "client_connections " << client_connections.load() << "\n"
            << "# HELP active_connections Current in-flight upstream proxy requests\n"
            << "# TYPE active_connections gauge\n"
            << "active_connections " << active_connections.load() << "\n"
            << "# HELP requests_per_second Average gateway throughput since start (RPS)\n"
            << "# TYPE requests_per_second gauge\n"
            << "requests_per_second " << current_rps() << "\n"
            << "# HELP worker_count Current AIMD concurrency limit\n"
            << "# TYPE worker_count gauge\n"
            << "worker_count " << worker_limit.load() << "\n"
            << "# HELP active_workers Currently executing worker jobs\n"
            << "# TYPE active_workers gauge\n"
            << "active_workers " << active_workers.load() << "\n"
            << "# HELP queue_length Total queued jobs across all domains\n"
            << "# TYPE queue_length gauge\n"
            << "queue_length " << queue_length.load() << "\n"
            << "# HELP latency_ms Gateway end-to-end latency percentiles\n"
            << "# TYPE latency_ms summary\n"
            << "latency_ms{quantile=\"0.50\"} " << pct.p50 << "\n"
            << "latency_ms{quantile=\"0.95\"} " << pct.p95 << "\n"
            << "latency_ms{quantile=\"0.99\"} " << pct.p99 << "\n";

        for (const auto& p : proxies) {
            std::lock_guard<std::mutex> lock(p->mtx);
            uint64_t tot = p->success_count + p->failure_count;
            double sr = (tot > 0) ? (static_cast<double>(p->success_count) / tot) : 1.0;
            std::string ep = label_escape(p->endpoint());
            out << "proxy_rtt_ms{proxy=\"" << prom_label(ep) << "\"} " << p->rtt_ms << "\n"
                << "proxy_success_rate{proxy=\"" << prom_label(ep) << "\"} " << sr << "\n"
                << "proxy_failure_count{proxy=\"" << prom_label(ep) << "\"} " << p->failure_count << "\n"
                << "proxy_active_connections{proxy=\"" << prom_label(ep) << "\"} " << p->active_connections.load() << "\n";
        }
        return out.str();
    }

private:
    TimePoint start_time_;
    TimePoint last_flush_;
    mutable std::mutex lat_mtx_;
    std::deque<double> latency_samples_;
    mutable std::mutex file_mtx_;
    std::ofstream csv_;
    std::ofstream json_;
};

} // namespace gateway
