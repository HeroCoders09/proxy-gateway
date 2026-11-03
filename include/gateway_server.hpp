#pragma once

#include "aimd.hpp"
#include "http.hpp"
#include "metrics.hpp"
#include "net.hpp"
#include "proxy_manager.hpp"
#include "security.hpp"
#include "scheduler.hpp"
#include "upstream_engine.hpp"
#include "types.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gateway {

class GatewayServer {
public:
    explicit GatewayServer(const GatewayConfig& cfg)
        : cfg_(cfg),
          metrics_(cfg),
          scheduler_(cfg),
          proxy_mgr_(cfg),
          aimd_(cfg),
          engine_(cfg, proxy_mgr_, cfg.io_threads) {
        engine_.set_callbacks([this](AsyncRequest& r) { on_upstream_done(r); },
                              [this](AsyncRequest& r, bool ok) { on_client_written(r, ok); });
        engine_.set_tunnel_callbacks([this](AsyncRequest& r) { on_tunnel_opened(r); },
                                     [this](AsyncRequest& r) { on_tunnel_closed(r); });
        aimd_.set_on_change([this]() {
            metrics_.worker_limit.store(aimd_.limit(), std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(slot_mtx_);
            slot_cv_.notify_all();
        });
    }

    ~GatewayServer() { stop(); }

    bool start() {
        SockAddr probe_addr;
        if (!parse_ip_literal(cfg_.listen_host, cfg_.listen_port, probe_addr)) {
            std::cerr << "Invalid listen_host '" << cfg_.listen_host << "' (an IPv4 or IPv6 address is required)\n";
            return false;
        }
        listen_fd_ = listen_on(cfg_.listen_host, cfg_.listen_port, 4096, /*nonblocking=*/true);
        if (listen_fd_ < 0) { std::perror("bind/listen"); return false; }
        sockaddr_storage bound{};
        socklen_t bl = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &bl) == 0) bound_port_ = sockaddr_get_port(bound);

        if (cfg_.admin_port > 0) {   // dedicated admin listener (127.0.0.1 unless configured otherwise)
            admin_fd_ = listen_on(cfg_.admin_host, cfg_.admin_port, 128, /*nonblocking=*/true);
            if (admin_fd_ < 0) {
                std::perror("admin bind/listen");
                ::close(listen_fd_);
                listen_fd_ = -1;
                return false;
            }
        }

        epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = nullptr;  // listener
        ::epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev);
        if (admin_fd_ >= 0) {
            ev.data.ptr = &admin_tag_;
            ::epoll_ctl(epfd_, EPOLL_CTL_ADD, admin_fd_, &ev);
        }

        running_.store(true);
        metrics_.worker_limit.store(aimd_.limit());
        proxy_mgr_.start_prober();
        engine_.start();
        dispatcher_thread_ = std::thread([this]() { dispatcher_loop(); });
        acceptor_thread_ = std::thread([this]() { event_loop(); });
        return true;
    }

    uint16_t port() const { return bound_port_; }
    int io_threads() const { return engine_.io_threads(); }
    MetricsManager& metrics() { return metrics_; }

    void stop() {
        if (!running_.exchange(false)) return;
        scheduler_.shutdown();
        {
            std::lock_guard<std::mutex> lk(slot_mtx_);
            slot_cv_.notify_all();
        }
        proxy_mgr_.stop_prober();
        if (acceptor_thread_.joinable()) acceptor_thread_.join();
        if (dispatcher_thread_.joinable()) dispatcher_thread_.join();
        engine_.stop();
        for (int i = 0; i < 400 && tunnels_.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        {
            std::lock_guard<std::mutex> lk(clients_mtx_);
            clients_.clear();
        }
        if (epfd_ >= 0) { ::close(epfd_); epfd_ = -1; }
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
        if (admin_fd_ >= 0) { ::close(admin_fd_); admin_fd_ = -1; }
        metrics_.flush();
    }

private:
    using ClientPtr = std::shared_ptr<ClientConn>;

    // ------------------------------------------------------------ client registry
    void register_client(const ClientPtr& c) {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        clients_[c.get()] = c;
        metrics_.client_connections.store(static_cast<int>(clients_.size()), std::memory_order_relaxed);
    }

    ClientPtr lookup_client(ClientConn* raw) {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(raw);
        return it == clients_.end() ? nullptr : it->second;
    }

    // Removes the connection from epoll and the registry; the fd closes when the last reference is dropped.
    void close_client(const ClientPtr& c) {
        if (epfd_ >= 0) ::epoll_ctl(epfd_, EPOLL_CTL_DEL, c->fd, nullptr);
        std::lock_guard<std::mutex> lk(clients_mtx_);
        clients_.erase(c.get());
        metrics_.client_connections.store(static_cast<int>(clients_.size()), std::memory_order_relaxed);
    }

    // Wait for the next readable event on this connection (one-shot).
    void arm(const ClientPtr& c) {
        c->partial.store(!c->buf.empty(), std::memory_order_relaxed);
        c->in_flight.store(false, std::memory_order_relaxed);
        c->handoff.fetch_add(1, std::memory_order_release);   // publish buf/peer_eof/... to the next owner
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLONESHOT;
        ev.data.ptr = c.get();
        if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, c->fd, &ev) < 0 && errno == ENOENT) {
            ::epoll_ctl(epfd_, EPOLL_CTL_ADD, c->fd, &ev);
        }
    }

    // ------------------------------------------------------------------ event loop
    void event_loop() {
        std::vector<epoll_event> events(256);
        auto last_sweep = Clock::now();
        while (running_.load()) {
            int n = ::epoll_wait(epfd_, events.data(), static_cast<int>(events.size()), 200);
            for (int i = 0; i < n; ++i) {
                if (events[i].data.ptr == nullptr) {
                    accept_clients(listen_fd_, false);
                } else if (events[i].data.ptr == &admin_tag_) {
                    accept_clients(admin_fd_, true);
                } else if (ClientPtr c = lookup_client(static_cast<ClientConn*>(events[i].data.ptr))) {
                    on_readable(c);
                }
            }
            if (Clock::now() - last_sweep > std::chrono::seconds(1)) {
                sweep_idle();
                last_sweep = Clock::now();
            }
        }
    }

    void accept_clients(int lfd, bool admin) {
        for (;;) {
            sockaddr_storage peer{};
            socklen_t pl = sizeof(peer);
            int cfd = ::accept4(lfd, reinterpret_cast<sockaddr*>(&peer), &pl, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (cfd < 0) {
                if (errno == EMFILE || errno == ENFILE) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return;
            }
            set_nodelay(cfd);
            bool full;
            {
                std::lock_guard<std::mutex> lk(clients_mtx_);
                full = clients_.size() >= cfg_.max_client_connections;
            }
            if (full) { ::close(cfd); continue; }
            auto c = std::make_shared<ClientConn>(cfd);
            c->admin_only = admin;
            c->peer_loopback = addr_is_loopback(peer);
            register_client(c);
            arm(c);
        }
    }

    void sweep_idle() {
        std::vector<ClientPtr> victims;
        {
            std::lock_guard<std::mutex> lk(clients_mtx_);
            for (auto& kv : clients_) {
                ClientConn& c = *kv.second;
                if (c.in_flight.load(std::memory_order_relaxed)) continue;
                int limit = c.partial.load(std::memory_order_relaxed) ? cfg_.client_header_timeout_ms
                                                                       : cfg_.client_idle_timeout_ms;
                if (c.idle_ms() > limit) victims.push_back(kv.second);
            }
        }
        for (auto& v : victims) close_client(v);
    }

    void on_readable(const ClientPtr& c) {
        (void)c->handoff.load(std::memory_order_acquire);     // take ownership (pairs with arm())
        char tmp[16384];
        const size_t hard_cap = cfg_.max_header_bytes + cfg_.max_body_bytes + 65536;
        for (;;) {
            ssize_t n = ::recv(c->fd, tmp, sizeof(tmp), 0);
            if (n > 0) {
                c->buf.append(tmp, static_cast<size_t>(n));
                c->touch();
                if (c->buf.size() > hard_cap) {
                    reply_inline(c, 413, "text/plain", "413 Payload Too Large\n", false);
                    return;
                }
                continue;
            }
            if (n == 0) { c->peer_eof = true; break; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            c->peer_eof = true;  // connection error: treat like EOF
            c->buf.clear();
            break;
        }
        process_buffer(c);
    }

    // Parses and handles as many complete requests as are buffered. Returns when the connection is
    // re-armed, closed, or handed to a worker (after which it must not be touched).
    void process_buffer(const ClientPtr& c) {
        for (;;) {
            if (c->buf.empty()) {
                if (c->peer_eof) close_client(c); else arm(c);
                return;
            }
            ParseResult pr = parse_http_request(c->buf, cfg_.max_header_bytes, cfg_.max_body_bytes);
            if (pr.status == ParseStatus::NeedMore) {
                if (pr.want_continue && !c->sent_continue) {
                    c->sent_continue = true;
                    send_nowait(c->fd, std::string("HTTP/1.1 100 Continue\r\n\r\n"));
                }
                if (c->peer_eof) close_client(c); else arm(c);
                return;
            }
            if (pr.status == ParseStatus::Bad) {
                reply_inline(c, pr.error_status, "text/plain", std::to_string(pr.error_status) + " " + pr.error + "\n", false);
                return;
            }
            c->buf.erase(0, pr.consumed);
            c->sent_continue = false;
            if (!handle_request(c, std::move(pr.req))) return;
        }
    }

    // Sends a small response from the calling thread. Returns true if the connection stays usable.
    bool reply_inline(const ClientPtr& c, int status, const std::string& ctype, const std::string& body,
                      bool keep, bool head_only = false, const std::string& extra_headers = std::string()) {
        std::string r = build_response(status, ctype, body, keep, head_only, extra_headers);
        bool ok = send_nowait(c->fd, r);  // never blocks the event loop; a client that is not reading is dropped
        if (!ok || !keep) { close_client(c); return false; }
        return true;
    }

    // Returns true if the request was answered inline and the connection remains open for more requests.
    bool handle_request(const ClientPtr& c, HttpRequest req) {
        const bool keep = req.keep_alive && !c->peer_eof;
        static const char* kMethods[] = {"GET", "HEAD", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "CONNECT"};
        bool known = false;
        for (const char* m : kMethods) if (req.method == m) known = true;
        if (!known) return reply_inline(c, 501, "text/plain", "501 Method Not Implemented\n", keep);

        if (c->admin_only) {   // the dedicated admin listener never proxies anything
            if (req.method == "CONNECT" || req.target.empty() || req.target[0] != '/')
                return reply_inline(c, 403, "text/plain", "403 Forbidden: this port serves admin endpoints only\n", keep);
            return handle_admin(c, req, keep);
        }

        Job job;
        job.method = req.method;

        if (req.method == "CONNECT") {
            std::string host;
            uint16_t port = 0;
            if (!parse_authority(req.target, host, port, -1)) {
                return reply_inline(c, 400, "text/plain", "400 CONNECT requires host:port\n", keep);
            }
            job.url = to_lower(req.target);
            job.domain = job.url;
            job.target_port = port;
        } else if (!req.target.empty() && req.target[0] == '/') {
            if (cfg_.admin_port > 0)
                return reply_inline(c, 404, "text/plain", "404 Not Found (admin endpoints are served on a separate port)\n", keep);
            if (!cfg_.admin_allow_remote && !c->peer_loopback)
                return reply_inline(c, 403, "text/plain", "403 Forbidden: admin endpoints are only available from localhost\n", keep);
            return handle_admin(c, req, keep);
        } else {
            UrlParts u;
            if (!parse_absolute_url(req.target, u)) {
                return reply_inline(c, 400, "text/plain", "400 Malformed URL (absolute http:// URL required)\n", keep);
            }
            if (u.https) {
                return reply_inline(c, 400, "text/plain", "400 Use the CONNECT method for https:// targets\n", keep);
            }
            job.url = req.target;
            job.domain = u.authority;
            job.path = u.path;
            job.target_port = u.port;
        }

        job.id = next_job_id_.fetch_add(1, std::memory_order_relaxed);
        job.client = c;
        job.headers = std::move(req.headers);
        job.body = std::move(req.body);
        job.keep_alive = keep;
        job.arrival_time = Clock::now();
        if (cfg_.queue_timeout_ms > 0) job.deadline = job.arrival_time + std::chrono::milliseconds(cfg_.queue_timeout_ms);

        c->in_flight.store(true, std::memory_order_relaxed);
        if (!scheduler_.enqueue(std::move(job))) {
            c->in_flight.store(false, std::memory_order_relaxed);
            return reply_inline(c, 503, "text/plain", "503 Service Unavailable: Domain Queue Overflow\n", keep);
        }
        metrics_.queue_length.store(scheduler_.total_queued(), std::memory_order_relaxed);
        return false;  // ownership moved to the scheduler/worker
    }

    static bool constant_time_equal(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        unsigned char d = 0;
        for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
        return d == 0;
    }

    bool handle_admin(const ClientPtr& c, const HttpRequest& req, bool keep) {
        std::string path = req.target.substr(0, req.target.find('?'));
        const bool head = (req.method == "HEAD");
        if (req.method != "GET" && !head) return reply_inline(c, 405, "text/plain", "405 Method Not Allowed\n", keep);
        const bool protect = !cfg_.admin_token.empty();
        if (protect && (path == "/metrics" || path == "/proxies")) {   // /health stays open (liveness probes), minimal
            const std::string* auth = header_find(req.headers, "authorization");
            if (!auth || !constant_time_equal(*auth, "Bearer " + cfg_.admin_token))
                return reply_inline(c, 401, "text/plain", "401 Unauthorized\n", keep, head, "WWW-Authenticate: Bearer\r\n");
        }
        if (path == "/metrics") {
            metrics_.pool_idle.store(engine_.idle_pool_size(), std::memory_order_relaxed);
            return reply_inline(c, 200, "text/plain; version=0.0.4", metrics_.render_prometheus(proxy_mgr_.proxies()), keep, head);
        }
        if (path == "/health" && protect) return reply_inline(c, 200, "application/json", "{\"status\":\"healthy\"}\n", keep, head);
        if (path == "/health") {
            std::string body = "{\"status\":\"healthy\",\"active_workers\":" + std::to_string(metrics_.active_workers.load()) +
                               ",\"aimd_limit\":" + std::to_string(aimd_.limit()) + "}\n";
            return reply_inline(c, 200, "application/json", body, keep, head);
        }
        if (path == "/proxies") {
            return reply_inline(c, 200, "application/json", proxy_mgr_.render_proxies_json() + "\n", keep, head);
        }
        return reply_inline(c, 404, "text/plain", "404 Not Found (admin paths: /metrics /health /proxies)\n", keep, head);
    }

    // Called when a job has been answered. Keeps the connection for the next request or closes it.
    void release_client(const ClientPtr& c, bool keep) {
        if (!keep) { close_client(c); return; }
        c->touch();
        c->in_flight.store(false, std::memory_order_relaxed);
        process_buffer(c);  // pipelined requests may already be buffered
    }

    // ----------------------------------------------------------------- dispatcher
    // A single thread moves jobs from the scheduler to the upstream engine without ever exceeding the AIMD
    // limit of in-flight requests. It performs no network I/O: all connect/send/read/retry/write work is done
    // by the engine's epoll threads, so a request occupies NO thread while it waits for the network.
    void dispatcher_loop() {
        std::vector<Job> expired;
        while (running_.load()) {
            {
                std::unique_lock<std::mutex> lk(slot_mtx_);
                while (running_.load() && inflight_ >= aimd_.limit()) {
                    slot_cv_.wait_for(lk, std::chrono::milliseconds(50));
                    lk.unlock();  // keep enforcing queue deadlines while all slots are busy
                    expired.clear();
                    scheduler_.expire(expired);
                    for (auto& e : expired) fail_queued_job(e);
                    lk.lock();
                }
            }
            if (!running_.load()) break;
            expired.clear();
            std::optional<Job> job = scheduler_.pop_blocking(expired);
            metrics_.queue_length.store(scheduler_.total_queued(), std::memory_order_relaxed);
            for (auto& e : expired) fail_queued_job(e);
            if (!job) continue;
            {
                std::lock_guard<std::mutex> lk(slot_mtx_);
                ++inflight_;
                metrics_.active_workers.store(inflight_, std::memory_order_relaxed);
            }
            engine_.submit(std::move(*job));
        }
    }

    void release_inflight() {
        {
            std::lock_guard<std::mutex> lk(slot_mtx_);
            --inflight_;
            metrics_.active_workers.store(inflight_, std::memory_order_relaxed);
        }
        slot_cv_.notify_one();
    }

    void fail_queued_job(Job& job) {
        metrics_.queue_timeouts.fetch_add(1, std::memory_order_relaxed);
        double latency = std::chrono::duration<double, std::milli>(Clock::now() - job.arrival_time).count();
        metrics_.record_completion(job.url, 504, "none", latency, false, false);
        std::string body = "504 Gateway Timeout: request waited too long in the rate-limit queue\n";
        bool ok = send_nowait(job.client->fd, build_response(504, "text/plain", body, job.keep_alive));
        release_client(job.client, ok && job.keep_alive);
    }

    static std::string error_text(int code) {
        if (code == 504) return "Gateway Timeout";
        if (code == 503) return "Service Unavailable (No Healthy Proxy)";
        return "Bad Gateway";
    }

    // ------------------------------------------------- engine callbacks (I/O threads)
    // Upstream phase finished (after any retries): account for it and prepare the reply.
    void on_upstream_done(AsyncRequest& r) {
        Job& job = r.job;
        ForwardResult& res = r.res;
        double latency = std::chrono::duration<double, std::milli>(Clock::now() - job.arrival_time).count();
        if (r.is_connect) {
            bool ok = r.tunnel_ok;
            int code = (res.http_status == 503 || res.http_status == 504) ? res.http_status : 502;
            aimd_.record_feedback(ok, !ok);
            metrics_.record_completion("CONNECT " + job.url, ok ? 200 : code, r.last_endpoint, latency, ok, res.timeout);
            if (ok) r.client_out = "HTTP/1.1 200 Connection Established\r\n\r\n" + r.tunnel_leftover;
            else r.client_out = build_response(code, "text/plain", std::to_string(code) + " " + error_text(code) + "\n", false);
            r.keep_after = false;
        } else {
            aimd_.record_feedback(res.success, !res.success);
            metrics_.record_completion(job.url, res.http_status, r.last_endpoint, latency, res.success, res.timeout);
            if (!res.raw_response.empty()) {  // forward the origin's response verbatim (also 5xx from the origin)
                r.client_out = std::move(res.raw_response);
                r.keep_after = job.keep_alive && res.framed && !res.close_header;
            } else {
                int code = (res.http_status == 503 || res.http_status == 504) ? res.http_status : 502;
                r.client_out = build_response(code, "text/plain", std::to_string(code) + " " + error_text(code) + "\n", job.keep_alive);
                r.keep_after = job.keep_alive;
            }
        }
        release_inflight();
    }

    // The reply has been written to the client (or the client failed/was too slow).
    void on_client_written(AsyncRequest& r, bool ok) {
        if (r.is_connect) {
            release_client(r.job.client, false);  // (a successful tunnel is taken over by the engine instead)
            return;
        }
        release_client(r.job.client, ok && r.keep_after);
    }

    // An established CONNECT tunnel is relayed by the engine's I/O loops (no thread per tunnel).
    void on_tunnel_opened(AsyncRequest& r) {
        unregister_only(r.job.client);
        r.proxy->active_connections.fetch_add(1, std::memory_order_relaxed);
        metrics_.tunnels_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.active_tunnels.fetch_add(1, std::memory_order_relaxed);
        tunnels_.fetch_add(1);
    }

    void on_tunnel_closed(AsyncRequest& r) {
        r.proxy->active_connections.fetch_sub(1, std::memory_order_relaxed);
        metrics_.active_tunnels.fetch_sub(1, std::memory_order_relaxed);
        tunnels_.fetch_sub(1);
    }

    void unregister_only(const ClientPtr& c) {
        if (epfd_ >= 0) ::epoll_ctl(epfd_, EPOLL_CTL_DEL, c->fd, nullptr);
        std::lock_guard<std::mutex> lk(clients_mtx_);
        clients_.erase(c.get());
        metrics_.client_connections.store(static_cast<int>(clients_.size()), std::memory_order_relaxed);
    }

    GatewayConfig cfg_;
    MetricsManager metrics_;
    JobScheduler scheduler_;
    ProxyManager proxy_mgr_;
    AIMDController aimd_;
    UpstreamEngine engine_;

    int listen_fd_ = -1;
    int admin_fd_ = -1;
    char admin_tag_ = 0;   // epoll tag of the admin listener
    int epfd_ = -1;
    uint16_t bound_port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> next_job_id_{1};
    std::atomic<int> tunnels_{0};
    std::thread acceptor_thread_;
    std::thread dispatcher_thread_;

    std::mutex clients_mtx_;
    std::unordered_map<ClientConn*, ClientPtr> clients_;

    std::mutex slot_mtx_;
    std::condition_variable slot_cv_;
    int inflight_ = 0;  // requests currently owned by the upstream engine (bounded by the AIMD limit)
};

} // namespace gateway
