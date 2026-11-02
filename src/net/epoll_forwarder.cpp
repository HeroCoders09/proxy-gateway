#include "net/epoll_forwarder.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <atomic>
#include <future>
#include <stdexcept>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/fd.hpp"
#include "net/http_response_parser.hpp"
#include "net/simple_forwarder.hpp"

namespace pg {
namespace {
using Steady = std::chrono::steady_clock;

struct Task {
  Job job;
  ProxyState proxy;
  std::promise<ForwardResult> promise;
  Steady::time_point started = Steady::now();
  Steady::time_point deadline{};
  bool completed = false;
};

struct Address {
  sockaddr_storage storage{};
  socklen_t length = 0;
  int family = AF_UNSPEC;
  int type = SOCK_STREAM;
  int protocol = 0;
};

enum class Stage { Connecting, Writing, Reading, Idle };

struct Upstream {
  Fd fd;
  ProxyId proxy_id = -1;
  Stage stage = Stage::Connecting;
  std::shared_ptr<Task> task;
  std::string request;
  size_t sent = 0;
  std::string response;
  Steady::time_point deadline{};
  Steady::time_point idle_since{};
};

ForwardResult failed(const Task& task, ErrorKind error, std::string detail) {
  ForwardResult result;
  result.error = error;
  result.error_detail = std::move(detail);
  result.proxy_id = task.proxy.id;
  result.latency_ms = std::chrono::duration<double, std::milli>(Steady::now() - task.started).count();
  return result;
}
}  // namespace

class EpollForwarder::Impl {
 public:
  Impl(int connect_timeout_ms, int response_timeout_ms, size_t max_response_bytes,
       size_t max_connections_per_proxy, int idle_timeout_ms, MetricsSink* metrics,
       size_t max_total_connections)
      : connect_timeout_ms_(connect_timeout_ms), response_timeout_ms_(response_timeout_ms),
        max_response_bytes_(max_response_bytes), max_per_proxy_(max_connections_per_proxy),
        idle_timeout_ms_(idle_timeout_ms), metrics_(metrics), max_total_connections_(max_total_connections),
        epoll_(::epoll_create1(EPOLL_CLOEXEC)),
        wake_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (connect_timeout_ms_ < 1 || response_timeout_ms_ < 1 || max_response_bytes_ < 1 ||
        max_per_proxy_ < 1 || idle_timeout_ms_ < 1 || max_total_connections_ < 1 ||
        max_per_proxy_ > max_total_connections_)
      throw std::invalid_argument("epoll forwarder limits and timeouts must be positive");
    if (!epoll_ || !wake_) throw std::runtime_error("cannot create epoll forwarder descriptors");
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wake_.get();
    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, wake_.get(), &ev) != 0)
      throw std::runtime_error("cannot register epoll wake descriptor");
    thread_ = std::thread([this] { run(); });
  }

  ~Impl() {
    stopping_.store(true);
    wake_loop();
    if (thread_.joinable()) thread_.join();
  }

  ForwardResult forward(const Job& job, const ProxyState& proxy) {
    auto task = std::make_shared<Task>();
    task->job = job;
    task->proxy = proxy;
    auto future = task->promise.get_future();
    {
      std::lock_guard<std::mutex> lock(queue_mu_);
      submitted_.push_back(task);
    }
    wake_loop();
    return future.get();
  }

 private:
  void wake_loop() {
    uint64_t one = 1;
    ssize_t rc;
    do { rc = ::write(wake_.get(), &one, sizeof one); } while (rc < 0 && errno == EINTR);
  }

  std::vector<Address> resolve(const ProxyState& proxy) {
    auto found = addresses_.find(proxy.id);
    if (found != addresses_.end()) return found->second;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (::getaddrinfo(proxy.host.c_str(), std::to_string(proxy.port).c_str(), &hints, &list) != 0)
      return {};
    std::vector<Address> result;
    for (addrinfo* ai = list; ai; ai = ai->ai_next) {
      if (ai->ai_addrlen > sizeof(sockaddr_storage)) continue;
      Address a;
      std::memcpy(&a.storage, ai->ai_addr, ai->ai_addrlen);
      a.length = static_cast<socklen_t>(ai->ai_addrlen);
      a.family = ai->ai_family;
      a.type = ai->ai_socktype;
      a.protocol = ai->ai_protocol;
      result.push_back(a);
    }
    ::freeaddrinfo(list);
    addresses_[proxy.id] = result;
    return result;
  }

  void complete(Upstream& conn, ForwardResult result, bool keep) {
    auto task = conn.task;
    conn.task.reset();
    if (keep) {
      conn.stage = Stage::Idle;
      if (metrics_) metrics_->add_gauge(Gauge::UpstreamIdleConnections, 1);
      conn.response.clear();
      conn.request.clear();
      conn.sent = 0;
      conn.idle_since = Steady::now();
      conn.deadline = {};
      idle_[conn.proxy_id].push_back(conn.fd.get());
      set_interest(conn, EPOLLIN | EPOLLRDHUP);
    } else {
      erase(conn.fd.get());
    }
    if (task && !task->completed) {
      task->completed = true;
      task->promise.set_value(std::move(result));
    }
  }

  void fail(Upstream& conn, ErrorKind kind, const char* detail) {
    if (metrics_ && kind == ErrorKind::ConnectTimeout) metrics_->inc(Counter::ConnectTimeouts);
    if (metrics_ && kind == ErrorKind::ResponseTimeout) metrics_->inc(Counter::ResponseTimeouts);
    if (conn.task) complete(conn, failed(*conn.task, kind, detail), false);
    else erase(conn.fd.get());
  }

  void set_interest(Upstream& conn, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = conn.fd.get();
    (void)::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, conn.fd.get(), &ev);
  }

  void erase(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    if (active_[it->second->proxy_id] > 0) --active_[it->second->proxy_id];
    if (metrics_) {
      metrics_->add_gauge(Gauge::UpstreamActiveConnections, -1);
      if (it->second->stage == Stage::Idle) metrics_->add_gauge(Gauge::UpstreamIdleConnections, -1);
    }
    auto& bucket = idle_[it->second->proxy_id];
    bucket.erase(std::remove(bucket.begin(), bucket.end(), fd), bucket.end());
    (void)::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, nullptr);
    connections_.erase(it);
  }

  bool start_connect(const std::shared_ptr<Task>& task) {
    auto candidates = resolve(task->proxy);
    if (candidates.empty()) {
      task->completed = true;
      task->promise.set_value(failed(*task, ErrorKind::ConnectFailed, "cannot resolve proxy host"));
      return false;
    }
    for (const auto& a : candidates) {
      Fd fd(::socket(a.family, a.type | SOCK_NONBLOCK | SOCK_CLOEXEC, a.protocol));
      if (!fd) continue;
      int one = 1;
      (void)::setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      int rc = ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&a.storage), a.length);
      if (rc != 0 && errno != EINPROGRESS) continue;
      auto conn = std::make_unique<Upstream>();
      conn->fd = std::move(fd);
      conn->proxy_id = task->proxy.id;
      conn->task = task;
      conn->request = build_proxy_request(task->job, true);
      conn->deadline = Steady::now() + std::chrono::milliseconds(connect_timeout_ms_);
      conn->stage = (rc == 0) ? Stage::Writing : Stage::Connecting;
      int raw = conn->fd.get();
      epoll_event ev{};
      ev.events = (rc == 0 ? EPOLLOUT : EPOLLOUT | EPOLLRDHUP);
      ev.data.fd = raw;
      if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, raw, &ev) != 0) continue;
      if (rc == 0) conn->deadline = Steady::now() + std::chrono::milliseconds(response_timeout_ms_);
      connections_.emplace(raw, std::move(conn));
      ++active_[task->proxy.id];
      if (metrics_) {
        metrics_->inc(Counter::ConnectionsOpened);
        metrics_->add_gauge(Gauge::UpstreamActiveConnections, 1);
      }
      return true;
    }
    task->completed = true;
    task->promise.set_value(failed(*task, ErrorKind::ConnectFailed, "connect failed"));
    return false;
  }

  bool evict_oldest_idle() {
    int oldest_fd = -1;
    Steady::time_point oldest = Steady::time_point::max();
    for (const auto& [proxy_id, bucket] : idle_) {
      (void)proxy_id;
      for (int fd : bucket) {
        auto it = connections_.find(fd);
        if (it != connections_.end() && it->second->stage == Stage::Idle && it->second->idle_since < oldest) {
          oldest = it->second->idle_since;
          oldest_fd = fd;
        }
      }
    }
    if (oldest_fd < 0) return false;
    erase(oldest_fd);
    return true;
  }

  void dispatch() {
    bool made_progress = true;
    while (made_progress && !pending_.empty()) {
      made_progress = false;
      const size_t count = pending_.size();
      for (size_t i = 0; i < count; ++i) {
        auto task = pending_.front();
        pending_.pop_front();
        auto& pool = idle_[task->proxy.id];
        while (!pool.empty() && connections_.find(pool.front()) == connections_.end()) pool.pop_front();
        while (!pool.empty()) {
          int candidate = pool.front();
          auto found = connections_.find(candidate);
          if (found == connections_.end()) { pool.pop_front(); continue; }
          char byte;
          ssize_t peek = ::recv(candidate, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
          if (peek == 0 || peek > 0 || (peek < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            pool.pop_front();
            erase(candidate);
            continue;
          }
          break;
        }
        if (!pool.empty()) {
          int fd = pool.front();
          pool.pop_front();
          auto& conn = *connections_.at(fd);
          task->started = Steady::now();
          if (metrics_) metrics_->add_gauge(Gauge::UpstreamIdleConnections, -1);
          conn.task = task;
          conn.stage = Stage::Writing;
          conn.request = build_proxy_request(task->job, true);
          conn.sent = 0;
          conn.response.clear();
          conn.deadline = Steady::now() + std::chrono::milliseconds(response_timeout_ms_);
          set_interest(conn, EPOLLOUT | EPOLLRDHUP);
          if (metrics_) metrics_->inc(Counter::ConnectionsReused);
          made_progress = true;
        } else if (active_[task->proxy.id] < max_per_proxy_ &&
                   connections_.size() >= max_total_connections_ && evict_oldest_idle()) {
          pending_.push_front(std::move(task));
          made_progress = true;
        } else if (active_[task->proxy.id] < max_per_proxy_ && connections_.size() < max_total_connections_) {
          task->started = Steady::now();
          start_connect(task);
          made_progress = true;
        } else {
          pending_.push_back(std::move(task));
        }
      }
    }
  }

  void drain_submissions() {
    uint64_t value;
    while (::read(wake_.get(), &value, sizeof value) > 0) {}
    std::deque<std::shared_ptr<Task>> incoming;
    {
      std::lock_guard<std::mutex> lock(queue_mu_);
      incoming.swap(submitted_);
    }
    while (!incoming.empty()) {
      pending_.push_back(std::move(incoming.front()));
      incoming.pop_front();
    }
    dispatch();
  }

  void handle_event(int fd, uint32_t flags) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    Upstream& conn = *it->second;
    if (conn.stage == Stage::Idle) {
      if (flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) erase(fd);  // stale pooled socket
      return;
    }
    if (flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
      if (conn.stage != Stage::Reading || !(flags & EPOLLIN)) {
        fail(conn, conn.stage == Stage::Connecting ? ErrorKind::ConnectFailed : ErrorKind::IoError,
             "upstream connection closed");
        dispatch();
        return;
      }
    }
    if (conn.stage == Stage::Connecting && (flags & EPOLLOUT)) {
      int error = 0;
      socklen_t len = sizeof error;
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
        fail(conn, ErrorKind::ConnectFailed, "connect failed");
        dispatch();
        return;
      }
      conn.stage = Stage::Writing;
      conn.deadline = Steady::now() + std::chrono::milliseconds(response_timeout_ms_);
    }
    if (conn.stage == Stage::Writing && (flags & EPOLLOUT)) {
      while (conn.sent < conn.request.size()) {
        ssize_t n = ::send(fd, conn.request.data() + conn.sent, conn.request.size() - conn.sent, MSG_NOSIGNAL);
        if (n > 0) {
          conn.sent += static_cast<size_t>(n);
          if (metrics_) metrics_->inc(Counter::BytesOut, static_cast<uint64_t>(n));
        }
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else { fail(conn, ErrorKind::IoError, "failed to send request to proxy"); dispatch(); return; }
      }
      if (conn.sent == conn.request.size()) {
        conn.stage = Stage::Reading;
        set_interest(conn, EPOLLIN | EPOLLRDHUP);
      }
    }
    if (conn.stage == Stage::Reading && (flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))) {
      char data[16384];
      bool eof = false;
      for (;;) {
        ssize_t n = ::recv(fd, data, sizeof data, 0);
        if (n > 0) {
          conn.response.append(data, static_cast<size_t>(n));
          if (metrics_) metrics_->inc(Counter::BytesIn, static_cast<uint64_t>(n));
          if (conn.response.size() > max_response_bytes_) {
            fail(conn, ErrorKind::BadResponse, "response too large"); dispatch(); return;
          }
        } else if (n == 0) { eof = true; break; }
        else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else { fail(conn, ErrorKind::IoError, "read from proxy failed"); dispatch(); return; }
      }
      auto parsed = parse_response(conn.response, conn.task->job.method == "HEAD", eof, max_response_bytes_);
      if (parsed.status == RespStatus::Complete) {
        ForwardResult result;
        result.status = parsed.response.status;
        result.headers = std::move(parsed.response.headers);
        result.body = std::move(parsed.response.body);
        result.proxy_id = conn.proxy_id;
        result.latency_ms = std::chrono::duration<double, std::milli>(Steady::now() - conn.task->started).count();
        result.error = ErrorKind::None;
        bool reusable = !eof && !parsed.response.upstream_wants_close;
        complete(conn, std::move(result), reusable);
        dispatch();
      } else if (parsed.status == RespStatus::Malformed || parsed.status == RespStatus::TooLarge || eof) {
        fail(conn, parsed.status == RespStatus::TooLarge ? ErrorKind::BadResponse : ErrorKind::BadResponse,
             parsed.error.empty() ? "connection closed before response completed" : parsed.error.c_str());
        dispatch();
      }
    }
  }

  void expire() {
    const auto now = Steady::now();
    std::vector<int> dead;
    std::vector<std::pair<int, ErrorKind>> timed_out;
    for (auto& [fd, ptr] : connections_) {
      if (ptr->stage == Stage::Idle) {
        if (now - ptr->idle_since > std::chrono::milliseconds(idle_timeout_ms_)) dead.push_back(fd);
      } else if (now >= ptr->deadline) {
        ErrorKind k = ptr->stage == Stage::Connecting ? ErrorKind::ConnectTimeout : ErrorKind::ResponseTimeout;
        timed_out.emplace_back(fd, k);
      }
    }
    for (const auto& [fd, kind] : timed_out) {
      auto it = connections_.find(fd);
      if (it != connections_.end())
        fail(*it->second, kind, kind == ErrorKind::ConnectTimeout ? "connect timed out" : "response timed out");
    }
    for (int fd : dead) erase(fd);
    dispatch();
  }

  void run() {
    epoll_event events[64];
    while (!stopping_.load()) {
      int n = ::epoll_wait(epoll_.get(), events, 64, 100);
      if (n < 0 && errno != EINTR) break;
      for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == wake_.get()) drain_submissions();
        else handle_event(events[i].data.fd, events[i].events);
      }
      expire();
    }
    for (auto& [fd, conn] : connections_) {
      (void)fd;
      if (conn->task && !conn->task->completed) {
        conn->task->completed = true;
        conn->task->promise.set_value(failed(*conn->task, ErrorKind::IoError, "forwarder stopped"));
      }
      if (metrics_) {
        metrics_->add_gauge(Gauge::UpstreamActiveConnections, -1);
        if (conn->stage == Stage::Idle) metrics_->add_gauge(Gauge::UpstreamIdleConnections, -1);
      }
    }
    connections_.clear();
    idle_.clear();
    active_.clear();
    for (auto& task : pending_) {
      if (!task->completed) {
        task->completed = true;
        task->promise.set_value(failed(*task, ErrorKind::IoError, "forwarder stopped"));
      }
    }
    std::deque<std::shared_ptr<Task>> submitted;
    {
      std::lock_guard<std::mutex> lock(queue_mu_);
      submitted.swap(submitted_);
    }
    for (auto& task : submitted) {
      if (!task->completed) {
        task->completed = true;
        task->promise.set_value(failed(*task, ErrorKind::IoError, "forwarder stopped"));
      }
    }
  }

  int connect_timeout_ms_;
  int response_timeout_ms_;
  size_t max_response_bytes_;
  size_t max_per_proxy_;
  int idle_timeout_ms_;
  MetricsSink* metrics_;
  size_t max_total_connections_;
  Fd epoll_;
  Fd wake_;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::mutex queue_mu_;
  std::deque<std::shared_ptr<Task>> submitted_;
  std::deque<std::shared_ptr<Task>> pending_;
  std::unordered_map<int, std::unique_ptr<Upstream>> connections_;
  std::unordered_map<ProxyId, std::deque<int>> idle_;
  std::unordered_map<ProxyId, size_t> active_;
  std::unordered_map<ProxyId, std::vector<Address>> addresses_;
};

EpollForwarder::EpollForwarder(int connect_timeout_ms, int response_timeout_ms,
                               size_t max_response_bytes, size_t max_connections_per_proxy,
                               int idle_timeout_ms, MetricsSink* metrics, size_t max_total_connections)
    : impl_(std::make_unique<Impl>(connect_timeout_ms, response_timeout_ms, max_response_bytes,
                                  max_connections_per_proxy, idle_timeout_ms, metrics, max_total_connections)) {}
EpollForwarder::~EpollForwarder() = default;
ForwardResult EpollForwarder::forward(const Job& job, const ProxyState& proxy) {
  return impl_->forward(job, proxy);
}

}  // namespace pg
