#pragma once
// Event-driven upstream engine.
//
// A small, fixed number of I/O threads (each running epoll) drive any number of in-flight requests as
// non-blocking state machines:
//
//   Connecting -> Sending -> Reading -> (retry on another proxy | finish) -> Writing (to the client)
//
// No thread is ever dedicated to a request: connect, send, response read, proxy retries, CONNECT
// handshakes, per-phase timeouts and the final write back to the (possibly slow) client are all
// handled by readiness events and timers. Established CONNECT tunnels are relayed by the same loops
// (non-blocking, back-pressured, half-close aware, idle timer), so a tunnel costs no thread either. The gateway's concurrency limit (AIMD) therefore bounds
// in-flight requests, not threads.

#include "metrics.hpp"
#include "net.hpp"
#include "proxy_manager.hpp"
#include "types.hpp"
#include <algorithm>
#include <cerrno>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gateway {

inline bool is_idempotent_method(const std::string& m) {
    return m == "GET" || m == "HEAD" || m == "OPTIONS" || m == "PUT" || m == "DELETE";
}

struct ForwardResult {
    bool success = false;
    bool timeout = false;
    bool request_sent = false;   // the whole request reached an upstream proxy (unsafe to blindly retry non-idempotent calls)
    bool framed = false;         // response length is known (client connection can stay open)
    bool close_header = false;   // response asks for connection close
    int http_status = 502;
    std::string raw_response;
    double rtt_ms = 0.0;
};

// ----------------------------------------------------------- idle connection pool
class UpstreamPool {
public:
    explicit UpstreamPool(int idle_ms = 30000, size_t max_per_key = 64, size_t max_total = 1024)
        : idle_ms_(idle_ms), max_per_key_(max_per_key), max_total_(max_total) {}

    size_t size() const { return total_.load(std::memory_order_relaxed); }

    // Closes connections that sat idle too long or that the peer has already closed (otherwise they would linger,
    // e.g. in CLOSE_WAIT, until somebody happened to reuse that key). Cheap; called by the I/O loops while the pool is non-empty.
    void reap() {
        std::vector<std::unique_ptr<Connection>> dead;   // closed outside the lock
        {
            std::lock_guard<std::mutex> lock(mtx_);
            auto now = Clock::now();
            for (auto it = idle_.begin(); it != idle_.end();) {
                auto& q = it->second;
                for (size_t i = 0; i < q.size();) {
                    bool old = (now - q[i]->last_used) >= std::chrono::milliseconds(idle_ms_);
                    if (old || !is_alive(q[i]->fd)) { dead.push_back(std::move(q[i])); q.erase(q.begin() + static_cast<long>(i)); --total_; }
                    else ++i;
                }
                it = q.empty() ? idle_.erase(it) : std::next(it);
            }
        }
    }

    std::unique_ptr<Connection> acquire(const std::string& key) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = idle_.find(key);
        if (it == idle_.end()) return nullptr;
        auto& q = it->second;
        while (!q.empty()) {
            auto conn = std::move(q.back());  // most recently used first
            q.pop_back();
            --total_;
            bool fresh = (Clock::now() - conn->last_used) < std::chrono::milliseconds(idle_ms_);
            if (fresh && is_alive(conn->fd)) {
                conn->state = ConnState::ACTIVE;
                return conn;
            }
        }
        idle_.erase(it);
        return nullptr;
    }

    void release(std::unique_ptr<Connection> conn) {
        if (!conn || conn->fd < 0) return;
        conn->state = ConnState::IDLE;
        conn->last_used = Clock::now();
        std::lock_guard<std::mutex> lock(mtx_);
        if (total_ >= max_total_ || max_per_key_ == 0) return;     // bounded: every idle connection holds an fd
        auto& q = idle_[conn->key];
        if (q.size() < max_per_key_) { q.push_back(std::move(conn)); ++total_; }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        idle_.clear();
        total_ = 0;
    }

private:
    static bool is_alive(int fd) {
        char c;
        ssize_t n = ::recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0) return false;   // peer closed
        if (n > 0) return false;    // unexpected data on an idle connection
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }

    int idle_ms_;
    size_t max_per_key_, max_total_;
    std::mutex mtx_;
    std::atomic<size_t> total_{0};
    std::unordered_map<std::string, std::vector<std::unique_ptr<Connection>>> idle_;
};

// ------------------------------------------------------------------ request state
struct AsyncRequest {
    enum class Phase { Connecting, Socks, Sending, Reading, Writing, Done };

    Job job;
    bool is_connect = false;
    std::string payload;                 // bytes sent to the upstream proxy (rebuilt for every attempt)
    std::string dest_host, dest_path;    // final destination (needed for SOCKS5 proxies)
    uint16_t dest_port = 0;
    bool dest_ok = false;
    // SOCKS5 handshake state
    int sstage = 0;                      // 0 = method selection, 1 = username/password, 2 = CONNECT request
    std::string sbuf, sin;
    size_t soff = 0;
    std::vector<int> tried;              // proxies already attempted
    std::vector<SockAddr> addrs;         // addresses of the current proxy (connect fail-over, e.g. ::1 then 127.0.0.1)
    size_t addr_try = 0;                 // index into addrs of the connect in progress
    std::shared_ptr<ProxyState> proxy;
    std::string last_endpoint = "none";
    ForwardResult res;                   // result of the latest attempt (final once the request finishes)

    // current attempt
    Phase phase = Phase::Connecting;
    std::unique_ptr<Connection> up;
    bool reused = false;
    size_t out_off = 0;
    std::unique_ptr<ResponseParser> parser;
    std::string connect_buf;             // CONNECT reply accumulator
    TimePoint attempt_start = Clock::now();
    int reg_fd = -1;                     // fd currently registered in the I/O thread's epoll

    // CONNECT outcome
    bool tunnel_ok = false;
    std::unique_ptr<Connection> tunnel_conn;
    std::string tunnel_leftover;

    // answer to the client (filled by the done callback)
    std::string client_out;
    size_t client_off = 0;
    bool keep_after = false;

    // timer bookkeeping
    std::multimap<TimePoint, std::shared_ptr<AsyncRequest>>::iterator timer_it;
    bool has_timer = false;
};

using AsyncPtr = std::shared_ptr<AsyncRequest>;

class UpstreamEngine {
public:
    using DoneFn = std::function<void(AsyncRequest&)>;             // upstream phase finished: fill client_out/keep_after
    using WrittenFn = std::function<void(AsyncRequest&, bool)>;    // client write finished (ok == fully written)

    UpstreamEngine(const GatewayConfig& cfg, ProxyManager& proxies, int io_threads)
        : cfg_(cfg), pm_(proxies), pool_(cfg.upstream_idle_timeout_ms, cfg.pool_max_idle_per_proxy, cfg.pool_max_idle_total) {
        reap_every_ms_ = std::clamp(cfg.upstream_idle_timeout_ms / 2, 100, 5000);
        int n = io_threads;
        if (n <= 0) {
            unsigned hw = std::thread::hardware_concurrency();
            n = static_cast<int>(std::max(1u, std::min(4u, hw ? hw : 1u)));
        }
        for (int i = 0; i < n; ++i) loops_.push_back(std::make_unique<Loop>());
    }

    ~UpstreamEngine() { stop(); }

    void set_callbacks(DoneFn done, WrittenFn written) {
        done_ = std::move(done);
        written_ = std::move(written);
    }

    // A CONNECT tunnel starts relaying / has ended (called on an I/O thread; the request still owns the client).
    void set_tunnel_callbacks(DoneFn opened, DoneFn closed) {
        tunnel_opened_ = std::move(opened);
        tunnel_closed_ = std::move(closed);
    }

    size_t idle_pool_size() const { return pool_.size(); }
    int io_threads() const { return static_cast<int>(loops_.size()); }

    void start() {
        running_.store(true);
        for (auto& lp : loops_) {
            lp->epfd = ::epoll_create1(EPOLL_CLOEXEC);
            lp->evfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.ptr = nullptr;  // wake-up fd
            ::epoll_ctl(lp->epfd, EPOLL_CTL_ADD, lp->evfd, &ev);
            Loop* raw = lp.get();
            lp->th = std::thread([this, raw]() { run(*raw); });
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        for (auto& lp : loops_) {
            uint64_t one = 1;
            [[maybe_unused]] ssize_t r = ::write(lp->evfd, &one, sizeof(one));
        }
        for (auto& lp : loops_) {
            if (lp->th.joinable()) lp->th.join();
            if (lp->epfd >= 0) ::close(lp->epfd);
            if (lp->evfd >= 0) ::close(lp->evfd);
            lp->epfd = lp->evfd = -1;
            lp->active.clear();
            lp->timers.clear();
            close_all_tunnels(*lp);
        }
        pool_.clear();
    }

    // Hands a job to an I/O thread. Never blocks.
    void submit(Job job) {
        auto r = std::make_shared<AsyncRequest>();
        r->is_connect = (job.method == "CONNECT");
        r->dest_ok = parse_authority(job.domain, r->dest_host, r->dest_port, job.target_port);
        if (!r->is_connect) {
            UrlParts u;
            r->dest_path = parse_absolute_url(job.url, u) ? u.path : job.path;
        }
        r->job = std::move(job);
        Loop& lp = *loops_[next_loop_.fetch_add(1, std::memory_order_relaxed) % loops_.size()];
        {
            std::lock_guard<std::mutex> lock(lp.inbox_mtx);
            lp.inbox.push_back(std::move(r));
        }
        uint64_t one = 1;
        [[maybe_unused]] ssize_t w = ::write(lp.evfd, &one, sizeof(one));
    }

private:
    // ---- CONNECT tunnel relay state (lives on one loop)
    struct Tunnel;
    struct TunSide { Tunnel* t; int idx; };
    struct TunDir {
        std::string buf;
        size_t off = 0;
        bool src_eof = false, shut = false;
        bool pending() const { return off < buf.size(); }
    };
    struct Tunnel {
        AsyncPtr req;                       // keeps the client connection alive
        std::unique_ptr<Connection> up;     // connection to the upstream proxy
        int fd[2] = {-1, -1};               // 0 = client, 1 = upstream
        TunDir dir[2];                      // dir[i]: reads fd[i], writes fd[1-i]
        TunSide side[2];
        uint32_t ev[2] = {0, 0};
        bool reg[2] = {false, false};
        TimePoint last = Clock::now();
        std::multimap<TimePoint, std::shared_ptr<Tunnel>>::iterator tit;
        bool has_timer = false;
        bool closed = false;
    };
    using TunnelPtr = std::shared_ptr<Tunnel>;

    struct Loop {
        std::unordered_map<TunSide*, TunnelPtr> tunnel_sides;
        std::multimap<TimePoint, TunnelPtr> tunnel_timers;
        TimePoint last_reap = Clock::now();
        int epfd = -1;
        int evfd = -1;
        std::thread th;
        std::mutex inbox_mtx;
        std::vector<AsyncPtr> inbox;
        std::unordered_map<AsyncRequest*, AsyncPtr> active;
        std::multimap<TimePoint, AsyncPtr> timers;
    };

    // HTTP proxies get the absolute URL; through a SOCKS5 tunnel the origin sees an ordinary origin-form request.
    static std::string build_forward_request(const Job& job, const std::string* origin_path) {
        std::string out;
        out.reserve(256 + job.body.size());
        out += job.method + " " + (origin_path ? *origin_path : job.url) + " HTTP/1.1\r\n";
        bool has_host = false;
        for (const auto& kv : job.headers) {
            std::string ln = to_lower(kv.first);
            if (is_hop_by_hop(ln)) continue;
            if (ln == "host") has_host = true;
            out += kv.first + ": " + kv.second + "\r\n";
        }
        if (!has_host) out += "Host: " + job.domain + "\r\n";
        bool body_method = (job.method == "POST" || job.method == "PUT" || job.method == "PATCH");
        if (!job.body.empty() || body_method) out += "Content-Length: " + std::to_string(job.body.size()) + "\r\n";
        out += "Connection: keep-alive\r\n\r\n";
        out += job.body;
        return out;
    }

    // ------------------------------------------------------------------ main loop
    void run(Loop& L) {
        std::vector<epoll_event> evs(256);
        while (running_.load()) {
            int timeout = -1;  // sleep until an event or the nearest timer: no periodic wake-ups when idle
            const bool pool_busy = pool_.size() > 0;   // idle pooled connections need reaping; an empty pool needs no wake-ups
            if (!L.timers.empty() || !L.tunnel_timers.empty() || pool_busy) {
                TimePoint nearest = TimePoint::max();
                if (!L.timers.empty()) nearest = std::min(nearest, L.timers.begin()->first);
                if (!L.tunnel_timers.empty()) nearest = std::min(nearest, L.tunnel_timers.begin()->first);
                if (pool_busy) nearest = std::min(nearest, L.last_reap + std::chrono::milliseconds(reap_every_ms_));
                auto d = std::chrono::duration_cast<std::chrono::microseconds>(nearest - Clock::now()).count();
                timeout = static_cast<int>(std::min<long long>(std::max<long long>((d + 999) / 1000, 0), 60000));
            }
            int n = ::epoll_wait(L.epfd, evs.data(), static_cast<int>(evs.size()), timeout);
            for (int i = 0; i < n; ++i) {
                if (evs[i].data.ptr == nullptr) {
                    uint64_t v;
                    [[maybe_unused]] ssize_t rr = ::read(L.evfd, &v, sizeof(v));
                    std::vector<AsyncPtr> batch;
                    {
                        std::lock_guard<std::mutex> lock(L.inbox_mtx);
                        batch.swap(L.inbox);
                    }
                    for (auto& r : batch) {
                        L.active[r.get()] = r;
                        start_attempt(L, r);
                    }
                    continue;
                }
                {
                    auto ts = L.tunnel_sides.find(static_cast<TunSide*>(evs[i].data.ptr));
                    if (ts != L.tunnel_sides.end()) {
                        TunnelPtr t = ts->second;
                        on_tunnel_event(L, t, evs[i].events);
                        continue;
                    }
                }
                auto it = L.active.find(static_cast<AsyncRequest*>(evs[i].data.ptr));
                if (it == L.active.end()) continue;  // stale event of an already finished request
                AsyncPtr r = it->second;
                on_event(L, r, evs[i].events);
            }
            auto now = Clock::now();
            while (!L.timers.empty() && L.timers.begin()->first <= now) {
                AsyncPtr r = L.timers.begin()->second;
                L.timers.erase(L.timers.begin());
                r->has_timer = false;
                on_timeout(L, r);
            }
            if (pool_.size() > 0 && now - L.last_reap >= std::chrono::milliseconds(reap_every_ms_)) {
                pool_.reap();
                L.last_reap = now;
            }
            while (!L.tunnel_timers.empty() && L.tunnel_timers.begin()->first <= now) {
                TunnelPtr t = L.tunnel_timers.begin()->second;
                L.tunnel_timers.erase(L.tunnel_timers.begin());
                t->has_timer = false;
                if (t->closed) continue;
                auto due = t->last + std::chrono::milliseconds(cfg_.tunnel_idle_timeout_ms);
                if (due <= now) { close_tunnel(L, t); continue; }    // idle too long
                t->tit = L.tunnel_timers.emplace(due, t);              // activity since: re-arm
                t->has_timer = true;
            }
        }
    }

    // ----------------------------------------------------------- epoll / timers
    void ep_set(Loop& L, const AsyncPtr& r, int fd, uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.ptr = r.get();
        if (r->reg_fd == fd) {
            ::epoll_ctl(L.epfd, EPOLL_CTL_MOD, fd, &ev);
            return;
        }
        if (r->reg_fd >= 0) ::epoll_ctl(L.epfd, EPOLL_CTL_DEL, r->reg_fd, nullptr);
        ::epoll_ctl(L.epfd, EPOLL_CTL_ADD, fd, &ev);
        r->reg_fd = fd;
    }

    void ep_clear(Loop& L, const AsyncPtr& r) {
        if (r->reg_fd >= 0) {
            ::epoll_ctl(L.epfd, EPOLL_CTL_DEL, r->reg_fd, nullptr);
            r->reg_fd = -1;
        }
    }

    void clear_timer(Loop& L, const AsyncPtr& r) {
        if (r->has_timer) {
            L.timers.erase(r->timer_it);
            r->has_timer = false;
        }
    }

    void set_timer(Loop& L, const AsyncPtr& r, int ms) {
        clear_timer(L, r);
        r->timer_it = L.timers.emplace(Clock::now() + std::chrono::milliseconds(ms), r);
        r->has_timer = true;
    }

    // ------------------------------------------------------------------ attempts
    bool can_retry(const AsyncRequest& r) const {
        if (static_cast<int>(r.tried.size()) >= cfg_.max_retries + 1) return false;
        if (r.res.request_sent && !is_idempotent_method(r.job.method)) return false;  // never replay a sent POST
        return true;
    }

    void start_attempt(Loop& L, const AsyncPtr& r) {
        auto proxy = pm_.select_proxy(r->tried);
        if (!proxy) {
            if (r->tried.empty()) { r->res = ForwardResult{}; r->res.http_status = 503; }
            finish(L, r);
            return;
        }
        r->tried.push_back(proxy->id);
        r->proxy = proxy;
        r->last_endpoint = proxy->endpoint();
        r->res = ForwardResult{};
        r->attempt_start = Clock::now();
        r->parser.reset();
        r->connect_buf.clear();
        r->up.reset();
        r->out_off = 0;
        bool socks = proxy->protocol == ProxyProtocol::SOCKS5;
        if (socks && !r->dest_ok) { attempt_failed(L, r, 502, false, false); return; }
        if (r->is_connect) r->payload = socks ? std::string() : ("CONNECT " + r->job.url + " HTTP/1.1\r\nHost: " + r->job.url + "\r\n\r\n");
        else r->payload = build_forward_request(r->job, socks ? &r->dest_path : nullptr);
        begin_connection(L, r, /*allow_pool=*/!r->is_connect);
    }

    static std::string pool_key(const AsyncRequest& r) {
        std::string k = std::to_string(r.proxy->id);
        if (r.proxy->protocol == ProxyProtocol::SOCKS5) k += "|" + r.dest_host + ":" + std::to_string(r.dest_port);
        return k;
    }

    // TCP connection to the proxy is up (fresh socket): SOCKS5 proxies need their handshake first.
    void tcp_ready(Loop& L, const AsyncPtr& r) {
        if (r->proxy->protocol == ProxyProtocol::SOCKS5) { socks_begin(L, r); return; }
        r->phase = AsyncRequest::Phase::Sending;
        try_send(L, r);
    }

    void begin_connection(Loop& L, const AsyncPtr& r, bool allow_pool) {
        r->reused = false;
        r->out_off = 0;
        if (allow_pool) {
            if (auto conn = pool_.acquire(pool_key(*r))) {
                r->up = std::move(conn);
                r->reused = true;
                r->phase = AsyncRequest::Phase::Sending;
                try_send(L, r);
                return;
            }
        }
        r->addrs.clear();
        r->addr_try = 0;
        if (!ProxyManager::address_list_of(*r->proxy, r->addrs)) { attempt_failed(L, r, 502, false, false); return; }
        open_next_addr(L, r);
    }

    // Starts a non-blocking connect to addrs[addr_try]; addresses that fail immediately are skipped.
    void open_next_addr(Loop& L, const AsyncPtr& r) {
        while (r->addr_try < r->addrs.size()) {
            const SockAddr& addr = r->addrs[r->addr_try];
            int fd = ::socket(addr.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0) { ++r->addr_try; continue; }
            set_nodelay(fd);
            int rc = ::connect(fd, addr.sa(), addr.len);
            if (rc < 0 && errno != EINPROGRESS) { ::close(fd); ++r->addr_try; continue; }
            r->up = std::make_unique<Connection>(fd, r->proxy->id, ConnState::CONNECTING);
            r->up->key = pool_key(*r);
            if (rc == 0) {
                note_addr_worked(*r);
                tcp_ready(L, r);
            } else {
                r->phase = AsyncRequest::Phase::Connecting;
                ep_set(L, r, fd, EPOLLOUT);
                set_timer(L, r, cfg_.connect_timeout_ms);
            }
            return;
        }
        attempt_failed(L, r, 502, false, false);   // every address failed
    }

    // The connect in progress failed (refused / unreachable / timed out): try the next address of the same proxy.
    bool connect_failover(Loop& L, const AsyncPtr& r) {
        if (r->addr_try + 1 >= r->addrs.size()) return false;
        ep_clear(L, r);
        clear_timer(L, r);
        r->up.reset();
        ++r->addr_try;
        open_next_addr(L, r);
        return true;
    }

    // Remember which address connected so later attempts try it first.
    static void note_addr_worked(const AsyncRequest& r) {
        if (r.addrs.size() < 2) return;
        size_t pref = r.proxy->addr_pref.load(std::memory_order_relaxed);
        r.proxy->addr_pref.store((pref + r.addr_try) % r.addrs.size(), std::memory_order_relaxed);
    }

    void try_send(Loop& L, const AsyncPtr& r) {
        int fd = r->up->fd;
        const std::string& p = r->payload;
        while (r->out_off < p.size()) {
            ssize_t n = ::send(fd, p.data() + r->out_off, p.size() - r->out_off, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0) { r->out_off += static_cast<size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                r->phase = AsyncRequest::Phase::Sending;
                ep_set(L, r, fd, EPOLLOUT);
                set_timer(L, r, cfg_.response_timeout_ms);
                return;
            }
            // write error: a stale pooled connection is retried once on a fresh one (nothing was delivered)
            if (r->reused) { ep_clear(L, r); r->up.reset(); begin_connection(L, r, false); return; }
            attempt_failed(L, r, 502, false, false);
            return;
        }
        // request fully sent: wait for the response
        r->res.request_sent = true;
        r->phase = AsyncRequest::Phase::Reading;
        if (!r->is_connect) r->parser = std::make_unique<ResponseParser>(r->job.method == "HEAD", cfg_.max_response_bytes);
        ep_set(L, r, fd, EPOLLIN);
        set_timer(L, r, cfg_.response_timeout_ms);
    }

    void on_event(Loop& L, const AsyncPtr& r, uint32_t events) {
        using P = AsyncRequest::Phase;
        switch (r->phase) {
            case P::Connecting: {
                int so_err = 0;
                socklen_t sl = sizeof(so_err);
                if (::getsockopt(r->up->fd, SOL_SOCKET, SO_ERROR, &so_err, &sl) < 0 || so_err != 0 ||
                    (events & EPOLLERR)) {
                    if (connect_failover(L, r)) return;
                    attempt_failed(L, r, 502, false, false);
                    return;
                }
                r->up->state = ConnState::ACTIVE;
                note_addr_worked(*r);
                tcp_ready(L, r);
                return;
            }
            case P::Socks:
                on_socks_event(L, r);
                return;
            case P::Sending:
                try_send(L, r);
                return;
            case P::Reading:
                if (r->is_connect) on_connect_reply(L, r); else on_response_data(L, r);
                return;
            case P::Writing:
                write_client(L, r);
                return;
            case P::Done:
                return;
        }
    }

    void on_timeout(Loop& L, const AsyncPtr& r) {
        using P = AsyncRequest::Phase;
        switch (r->phase) {
            case P::Connecting:
                if (connect_failover(L, r)) return;
                attempt_failed(L, r, 504, true, false);
                return;
            case P::Socks: attempt_failed(L, r, 504, true, false); return;
            case P::Sending: attempt_failed(L, r, 504, true, false); return;
            case P::Reading: attempt_failed(L, r, 504, true, true); return;
            case P::Writing: complete_write(L, r, false); return;  // client too slow / gone
            case P::Done: return;
        }
    }

    // --------------------------------------------------------------- HTTP response
    void on_response_data(Loop& L, const AsyncPtr& r) {
        char buf[16384];
        for (int budget = 8; budget > 0; --budget) {
            ssize_t n = ::recv(r->up->fd, buf, sizeof(buf), MSG_DONTWAIT);
            ReadStatus st;
            if (n > 0) {
                st = r->parser->feed(buf, static_cast<size_t>(n));
                if (st == ReadStatus::NeedMore) continue;
            } else if (n == 0) {
                st = r->parser->finish_eof();
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;  // level-triggered: epoll wakes us when more arrives
            } else {
                st = ReadStatus::Error;
            }
            handle_parse_result(L, r, st);
            return;
        }
    }

    void handle_parse_result(Loop& L, const AsyncPtr& r, ReadStatus st) {
        if (st == ReadStatus::Ok) { attempt_succeeded(L, r); return; }
        // Stale keep-alive connection (closed by the proxy while idle): replay idempotent requests once on a fresh one.
        if (r->reused && !r->parser->data.any_bytes && is_idempotent_method(r->job.method)) {
            ep_clear(L, r);   // always deregister before the fd is closed
            r->up.reset();
            clear_timer(L, r);
            r->parser.reset();
            r->res.request_sent = false;
            begin_connection(L, r, false);
            return;
        }
        attempt_failed(L, r, 502, false, true);
    }

    void attempt_succeeded(Loop& L, const AsyncPtr& r) {
        ResponseData& rd = r->parser->data;
        double rtt = std::chrono::duration<double, std::milli>(Clock::now() - r->attempt_start).count();
        bool reusable = rd.framed && !rd.close_header && !rd.peer_closed && !rd.extra_bytes;
        r->res.http_status = rd.status;
        r->res.framed = rd.framed;
        r->res.close_header = rd.close_header;
        r->res.success = (rd.status >= 200 && rd.status < 500);
        r->res.raw_response = std::move(rd.raw);
        r->res.rtt_ms = rtt;
        ep_clear(L, r);
        clear_timer(L, r);
        if (reusable) pool_.release(std::move(r->up)); else r->up.reset();
        r->parser.reset();
        pm_.record_outcome(r->proxy->id, r->res.success, rtt);
        if (r->res.success || !can_retry(*r)) { finish(L, r); return; }
        start_attempt(L, r);  // origin answered 5xx: try another proxy
    }

    void attempt_failed(Loop& L, const AsyncPtr& r, int status, bool timeout, bool request_sent) {
        double rtt = std::chrono::duration<double, std::milli>(Clock::now() - r->attempt_start).count();
        ep_clear(L, r);
        clear_timer(L, r);
        r->up.reset();
        r->parser.reset();
        r->res = ForwardResult{};
        r->res.http_status = status;
        r->res.timeout = timeout;
        r->res.request_sent = request_sent;
        r->res.rtt_ms = rtt;
        pm_.record_outcome(r->proxy->id, false, rtt);
        if (!can_retry(*r)) { finish(L, r); return; }
        start_attempt(L, r);
    }

    // ------------------------------------------------------------- CONNECT reply
    void on_connect_reply(Loop& L, const AsyncPtr& r) {
        char buf[4096];
        for (int budget = 4; budget > 0; --budget) {
            ssize_t n = ::recv(r->up->fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0) {
                r->connect_buf.append(buf, static_cast<size_t>(n));
            } else if (n == 0) {
                attempt_failed(L, r, 502, false, true);
                return;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (r->connect_buf.find("\r\n\r\n") == std::string::npos) return;
            } else {
                attempt_failed(L, r, 502, false, true);
                return;
            }
            size_t he = r->connect_buf.find("\r\n\r\n");
            if (he == std::string::npos) {
                if (r->connect_buf.size() > 16384) { attempt_failed(L, r, 502, false, true); return; }
                continue;
            }
            const std::string& b = r->connect_buf;
            int st = 0;
            if (!parse_status_line(b.data(), b.find("\r\n"), st) || st != 200) { attempt_failed(L, r, 502, false, true); return; }
            tunnel_established(L, r, b.substr(he + 4));
            return;
        }
    }

    void tunnel_established(Loop& L, const AsyncPtr& r, std::string leftover) {
        double rtt = std::chrono::duration<double, std::milli>(Clock::now() - r->attempt_start).count();
        ep_clear(L, r);
        clear_timer(L, r);
        r->tunnel_ok = true;
        r->tunnel_leftover = std::move(leftover);
        r->tunnel_conn = std::move(r->up);
        r->res = ForwardResult{};
        r->res.success = true;
        r->res.http_status = 200;
        r->res.request_sent = true;
        r->res.rtt_ms = rtt;
        pm_.record_outcome(r->proxy->id, true, rtt);
        finish(L, r);
    }

    // ------------------------------------------------------------- SOCKS5 client (to the upstream proxy)
    void socks_begin(Loop& L, const AsyncPtr& r) {
        r->phase = AsyncRequest::Phase::Socks;
        r->sstage = 0;
        r->sbuf = socks5_greeting(has_credentials(*r->proxy));
        r->soff = 0;
        r->sin.clear();
        socks_flush(L, r);
    }

    static bool has_credentials(const ProxyState& p) { return !p.username.empty() || !p.password.empty(); }

    // Queues the next SOCKS5 message and sends it.
    void socks_next(Loop& L, const AsyncPtr& r, int stage, std::string msg) {
        r->sstage = stage;
        r->sin.clear();
        r->sbuf = std::move(msg);
        r->soff = 0;
        socks_flush(L, r);
    }

    void socks_flush(Loop& L, const AsyncPtr& r) {
        int fd = r->up->fd;
        while (r->soff < r->sbuf.size()) {
            ssize_t n = ::send(fd, r->sbuf.data() + r->soff, r->sbuf.size() - r->soff, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0) { r->soff += static_cast<size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ep_set(L, r, fd, EPOLLOUT);
                set_timer(L, r, cfg_.connect_timeout_ms);
                return;
            }
            attempt_failed(L, r, 502, false, false);
            return;
        }
        ep_set(L, r, fd, EPOLLIN);                    // request sent: wait for the proxy's answer
        set_timer(L, r, cfg_.connect_timeout_ms);
    }

    void on_socks_event(Loop& L, const AsyncPtr& r) {
        if (r->soff < r->sbuf.size()) { socks_flush(L, r); return; }
        char buf[512];
        for (int budget = 4; budget > 0; --budget) {
            ssize_t n = ::recv(r->up->fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0) r->sin.append(buf, static_cast<size_t>(n));
            else if (n == 0) { attempt_failed(L, r, 502, false, false); return; }
            else if (errno == EINTR) continue;
            else if (errno != EAGAIN && errno != EWOULDBLOCK) { attempt_failed(L, r, 502, false, false); return; }
            if (r->sstage == 0) {
                int st = socks5_parse_method_reply(r->sin);
                if (st < 0) { attempt_failed(L, r, 502, false, false); return; }
                if (st == 0) { if (n < 0) return; continue; }
                std::string req;
                if (r->sin.size() > 2) { attempt_failed(L, r, 502, false, false); return; }   // extra bytes
                if (st == 2) {   // the proxy wants username/password (RFC 1929)
                    if (!has_credentials(*r->proxy) || !socks5_auth_request(r->proxy->username, r->proxy->password, req)) {
                        attempt_failed(L, r, 502, false, false);
                        return;
                    }
                    socks_next(L, r, 1, std::move(req));
                    return;
                }
                if (!socks5_connect_request(r->dest_host, r->dest_port, req)) { attempt_failed(L, r, 502, false, false); return; }
                socks_next(L, r, 2, std::move(req));
                return;
            }
            if (r->sstage == 1) {
                int st = socks5_parse_auth_reply(r->sin);
                if (st < 0) { attempt_failed(L, r, 502, false, false); return; }   // bad credentials / malformed
                if (st == 0) { if (n < 0) return; continue; }
                std::string req;
                if (r->sin.size() > 2 || !socks5_connect_request(r->dest_host, r->dest_port, req)) {
                    attempt_failed(L, r, 502, false, false);
                    return;
                }
                socks_next(L, r, 2, std::move(req));
                return;
            }
            size_t total = 0;
            int rep = 0;
            int st = socks5_parse_reply(r->sin, total, rep);
            if (st < 0) { attempt_failed(L, r, 502, false, false); return; }
            if (st == 0) { if (n < 0) return; continue; }
            std::string extra = r->sin.substr(total);
            r->sin.clear();
            if (r->is_connect) { tunnel_established(L, r, std::move(extra)); return; }
            if (!extra.empty()) { attempt_failed(L, r, 502, false, false); return; }  // a server must not speak first
            clear_timer(L, r);
            r->phase = AsyncRequest::Phase::Sending;
            r->out_off = 0;
            try_send(L, r);
            return;
        }
    }

    // ------------------------------------------------------------- finishing / client
    void finish(Loop& L, const AsyncPtr& r) {
        ep_clear(L, r);
        clear_timer(L, r);
        r->up.reset();
        r->parser.reset();
        if (done_) done_(*r);
        r->phase = AsyncRequest::Phase::Writing;
        r->client_off = 0;
        write_client(L, r);
    }

    void write_client(Loop& L, const AsyncPtr& r) {
        int fd = r->job.client->fd;
        while (r->client_off < r->client_out.size()) {
            ssize_t n = ::send(fd, r->client_out.data() + r->client_off, r->client_out.size() - r->client_off,
                               MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0) { r->client_off += static_cast<size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ep_set(L, r, fd, EPOLLOUT);      // slow client: wait for writability without blocking anyone
                set_timer(L, r, 10000);
                return;
            }
            complete_write(L, r, false);
            return;
        }
        complete_write(L, r, true);
    }

    void complete_write(Loop& L, const AsyncPtr& r, bool ok) {
        ep_clear(L, r);
        clear_timer(L, r);
        r->phase = AsyncRequest::Phase::Done;
        L.active.erase(r.get());
        if (r->is_connect && r->tunnel_ok && ok) { start_tunnel(L, r); return; }
        if (written_) written_(*r, ok);
    }

    // ------------------------------------------------------------------ CONNECT tunnels
    void start_tunnel(Loop& L, const AsyncPtr& r) {
        auto t = std::make_shared<Tunnel>();
        t->req = r;
        t->up = std::move(r->tunnel_conn);
        t->fd[0] = r->job.client->fd;
        t->fd[1] = t->up->fd;
        t->side[0] = {t.get(), 0};
        t->side[1] = {t.get(), 1};
        set_nonblocking(t->fd[0]);
        set_nonblocking(t->fd[1]);
        t->last = Clock::now();
        if (tunnel_opened_) tunnel_opened_(*r);
        L.tunnel_sides[&t->side[0]] = t;
        L.tunnel_sides[&t->side[1]] = t;
        t->tit = L.tunnel_timers.emplace(t->last + std::chrono::milliseconds(cfg_.tunnel_idle_timeout_ms), t);
        t->has_timer = true;
        pump_tunnel(L, t);
    }

    // Moves what can be moved without blocking, then (re)computes epoll interest. Closes the tunnel when finished.
    void pump_tunnel(Loop& L, const TunnelPtr& t) {
        char tmp[16384];
        bool ok = true;
        for (int round = 0; ok && round < 4; ++round) {
            bool progress = false;
            for (int i = 0; i < 2 && ok; ++i) {
                TunDir& d = t->dir[i];
                int src = t->fd[i], dst = t->fd[1 - i];
                if (!d.pending() && !d.src_eof) {
                    ssize_t n = ::recv(src, tmp, sizeof(tmp), MSG_DONTWAIT);
                    if (n > 0) { d.buf.assign(tmp, static_cast<size_t>(n)); d.off = 0; t->last = Clock::now(); progress = true; }
                    else if (n == 0) { d.src_eof = true; progress = true; }
                    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ok = false;
                }
                if (ok && d.pending()) {
                    ssize_t n = ::send(dst, d.buf.data() + d.off, d.buf.size() - d.off, MSG_NOSIGNAL | MSG_DONTWAIT);
                    if (n > 0) {
                        d.off += static_cast<size_t>(n);
                        if (d.off >= d.buf.size()) { d.buf.clear(); d.off = 0; }
                        t->last = Clock::now();
                        progress = true;
                    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ok = false;
                }
                if (ok && d.src_eof && !d.pending() && !d.shut) {
                    ::shutdown(dst, SHUT_WR);
                    d.shut = true;
                    progress = true;
                }
            }
            if (!progress) break;
        }
        if (!ok || (t->dir[0].shut && t->dir[1].shut)) { close_tunnel(L, t); return; }
        for (int i = 0; i < 2; ++i) {
            uint32_t want = 0;
            if (!t->dir[i].src_eof && !t->dir[i].pending()) want |= EPOLLIN;
            if (t->dir[1 - i].pending()) want |= EPOLLOUT;
            if (t->reg[i] && t->ev[i] == want) continue;
            epoll_event ev{};
            ev.events = want;
            ev.data.ptr = &t->side[i];
            ::epoll_ctl(L.epfd, t->reg[i] ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, t->fd[i], &ev);
            t->reg[i] = true;
            t->ev[i] = want;
        }
    }

    void on_tunnel_event(Loop& L, const TunnelPtr& t, uint32_t events) {
        if (t->closed) return;
        if (events & EPOLLERR) { close_tunnel(L, t); return; }
        pump_tunnel(L, t);
        // HUP with nothing left to move in either direction means no further I/O is possible: don't spin on it.
        if (!t->closed && (events & EPOLLHUP) && !(events & EPOLLIN)) close_tunnel(L, t);
    }

    void close_tunnel(Loop& L, const TunnelPtr& t) {
        if (t->closed) return;
        t->closed = true;
        for (int i = 0; i < 2; ++i)   // deregister before the fds are closed
            if (t->reg[i]) { ::epoll_ctl(L.epfd, EPOLL_CTL_DEL, t->fd[i], nullptr); t->reg[i] = false; }
        if (t->has_timer) { L.tunnel_timers.erase(t->tit); t->has_timer = false; }
        L.tunnel_sides.erase(&t->side[0]);
        L.tunnel_sides.erase(&t->side[1]);
        if (tunnel_closed_) tunnel_closed_(*t->req);
        t->up.reset();             // closes the upstream socket
        t->req.reset();            // drops the last reference to the client (closes its socket)
    }

    void close_all_tunnels(Loop& L) {
        std::vector<TunnelPtr> all;
        for (auto& kv : L.tunnel_sides) if (kv.first == &kv.second->side[0]) all.push_back(kv.second);
        for (auto& t : all) close_tunnel(L, t);
        L.tunnel_sides.clear();
        L.tunnel_timers.clear();
    }

    GatewayConfig cfg_;
    ProxyManager& pm_;
    UpstreamPool pool_;
    DoneFn done_;
    WrittenFn written_;
    DoneFn tunnel_opened_, tunnel_closed_;
    std::vector<std::unique_ptr<Loop>> loops_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> next_loop_{0};
    int reap_every_ms_ = 5000;
};

} // namespace gateway
