#include "ingress/listener.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include "common/log.hpp"
#include "ingress/http_parser.hpp"

namespace pg {

namespace {
constexpr int kReplyTimeoutMs = 5000;

// After answering a request we are rejecting, the client may still be sending (for example an
// oversized header). Closing with unread data makes Linux send a TCP reset, which can wipe the
// 400 out of the client's receive buffer before it is read. So: half-close, then drain briefly.
void lingering_close(int fd) {
  ::shutdown(fd, SHUT_WR);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  char sink[8192];
  size_t drained = 0;
  while (drained < (1u << 20)) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) return;
    pollfd p{fd, POLLIN, 0};
    int rc = ::poll(&p, 1, static_cast<int>(left));
    if (rc < 0 && errno == EINTR) continue;
    if (rc <= 0) return;
    ssize_t n = ::recv(fd, sink, sizeof sink, 0);
    if (n <= 0) return;  // peer closed (or error): done
    drained += static_cast<size_t>(n);
  }
}
}  // namespace

Listener::Listener(const GatewayConfig& cfg, MetricsSink& metrics, Handler handler)
    : cfg_(cfg), metrics_(metrics), handler_(std::move(handler)) {}

Listener::~Listener() { stop(); }

void Listener::start() {
  Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd) throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));

  int one = 1;
  ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg_.listen_port);
  if (::inet_pton(AF_INET, cfg_.listen_host.c_str(), &addr.sin_addr) != 1) {
    throw std::runtime_error("listen.host must be an IPv4 address, got '" + cfg_.listen_host + "'");
  }
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    throw std::runtime_error("cannot bind " + cfg_.listen_host + ":" + std::to_string(cfg_.listen_port) +
                             ": " + std::strerror(errno));
  }
  if (::listen(fd.get(), 512) != 0) {
    throw std::runtime_error(std::string("listen() failed: ") + std::strerror(errno));
  }
  socklen_t len = sizeof addr;
  ::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &len);
  bound_port_ = ntohs(addr.sin_port);
  listen_fd_ = std::move(fd);

  stop_ = false;
  for (int i = 0; i < cfg_.ingress.threads; ++i) workers_.emplace_back([this] { worker_loop(); });
  accept_thread_ = std::thread([this] { accept_loop(); });
}

void Listener::stop() {
  if (stop_.exchange(true)) {
    // Already stopping/stopped; still make sure threads are joined if start() ran.
  }
  cv_.notify_all();
  if (accept_thread_.joinable()) accept_thread_.join();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();
  {
    std::lock_guard<std::mutex> lk(mu_);
    pending_.clear();  // Fd destructors close any connections we never served
  }
  listen_fd_.reset();
}

void Listener::accept_loop() {
  while (!stop_) {
    pollfd p{listen_fd_.get(), POLLIN, 0};
    int rc = ::poll(&p, 1, 200);
    if (rc < 0) {
      if (errno == EINTR) continue;
      log_error(std::string("poll on listen socket failed: ") + std::strerror(errno));
      break;
    }
    if (rc == 0) continue;

    Fd client(::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC));
    if (!client) {
      // ECONNABORTED, EMFILE, ... : never crash the accept loop.
      if (errno == EMFILE || errno == ENFILE) {
        log_warn("out of file descriptors in accept(); backing off");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      continue;
    }
    int one = 1;
    ::setsockopt(client.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    bool queued = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (pending_.size() < static_cast<size_t>(cfg_.ingress.pending_connections)) {
        pending_.push_back(std::move(client));
        queued = true;
      }
    }
    if (queued) {
      cv_.notify_one();
    } else {
      // Overload: answer 503 immediately instead of letting connections pile up.
      write_response(client.get(), make_error_response(503, "gateway overloaded"), false, 200);
    }
  }
}

void Listener::worker_loop() {
  for (;;) {
    Fd client;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stop_ || !pending_.empty(); });
      if (stop_) return;
      client = std::move(pending_.front());
      pending_.pop_front();
    }
    try {
      handle_connection(std::move(client));
    } catch (const std::exception& e) {
      log_error(std::string("unexpected error handling connection: ") + e.what());
    } catch (...) {
      log_error("unexpected unknown error handling connection");
    }
  }
}

void Listener::handle_connection(Fd client) {
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::milliseconds(cfg_.ingress.read_timeout_ms);
  ParseLimits limits;
  limits.max_header_bytes = cfg_.ingress.max_header_bytes;
  limits.max_body_bytes = cfg_.ingress.max_body_bytes;

  std::string buf;
  char chunk[8192];
  ParseResult pr;

  for (;;) {
    pr = parse_request(buf, limits);
    if (pr.status != ParseStatus::NeedMore) break;

    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
    if (left <= 0) {
      if (buf.empty()) return;  // idle connection that never sent anything: just close
      metrics_.inc(Counter::BadRequests);
      write_response(client.get(), make_error_response(400, "request timed out"), false, kReplyTimeoutMs);
      return;
    }
    pollfd p{client.get(), POLLIN, 0};
    int rc = ::poll(&p, 1, static_cast<int>(left));
    if (rc < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (rc == 0) continue;  // loop re-checks the deadline

    ssize_t n = ::recv(client.get(), chunk, sizeof chunk, 0);
    if (n > 0) {
      buf.append(chunk, static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    // EOF or reset.
    if (n == 0 && !buf.empty()) {
      metrics_.inc(Counter::BadRequests);
      write_response(client.get(), make_error_response(400, "incomplete request"), false, kReplyTimeoutMs);
    }
    return;
  }

  if (pr.status == ParseStatus::Malformed || pr.status == ParseStatus::TooLarge) {
    metrics_.inc(Counter::BadRequests);
    write_response(client.get(), make_error_response(400, pr.reason), false, kReplyTimeoutMs);
    lingering_close(client.get());
    return;
  }

  // Complete: turn it into a Job.
  metrics_.inc(Counter::RequestsTotal);
  Job job;
  job.id = next_job_id_.fetch_add(1);
  job.url = std::move(pr.request.url);
  job.domain = pr.request.host;
  job.method = std::move(pr.request.method);
  job.headers = std::move(pr.request.headers);
  job.body = std::move(pr.request.body);
  job.enqueue_time = clock::now();
  job.client_fd = client.get();
  job.target_host = std::move(pr.request.host);
  job.target_port = pr.request.port;
  job.target_path = std::move(pr.request.path);

  const bool head_only = (job.method == "HEAD");
  HttpResponse resp;
  try {
    resp = handler_(job);
  } catch (const std::exception& e) {
    log_error(std::string("handler threw: ") + e.what());
    resp = make_error_response(502, "internal gateway error");
  } catch (...) {
    resp = make_error_response(502, "internal gateway error");
  }
  write_response(client.get(), resp, head_only, kReplyTimeoutMs);
}

}  // namespace pg
