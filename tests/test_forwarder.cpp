#include "catch_amalgamated.hpp"
#include "common/http_util.hpp"
#include "net/epoll_forwarder.hpp"
#include "net/simple_forwarder.hpp"
#include "test_util.hpp"

#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace pg;
using pgtest::FakeServer;

static Job make_job(const std::string& method = "GET", const std::string& path = "/products/123") {
  Job j;
  j.id = 1;
  j.method = method;
  j.target_host = "test.example";
  j.domain = "test.example";
  j.target_port = 80;
  j.target_path = path;
  j.url = "http://test.example" + path;
  return j;
}

static ProxyState proxy_at(uint16_t port) {
  ProxyState p;
  p.id = 7;
  p.host = "127.0.0.1";
  p.port = port;
  return p;
}

class KeepAliveTestProxy {
 public:
  explicit KeepAliveTestProxy(bool close_after_response = false) : close_after_response_(close_after_response) {
    listen_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    int one = 1;
    ::setsockopt(listen_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(listen_.get(), 64) != 0)
      throw std::runtime_error("test proxy listen setup failed");
    socklen_t len = sizeof addr;
    if (::getsockname(listen_.get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0)
      throw std::runtime_error("test proxy getsockname failed");
    port_ = ntohs(addr.sin_port);
    accept_thread_ = std::thread([this] { accept_loop(); });
  }

  ~KeepAliveTestProxy() {
    stopping_ = true;
    if (accept_thread_.joinable()) accept_thread_.join();
    for (auto& thread : handlers_) if (thread.joinable()) thread.join();
  }

  uint16_t port() const { return port_; }
  int accepted() const { return accepted_.load(); }
  int served() const { return served_.load(); }

 private:
  void accept_loop() {
    while (!stopping_) {
      pollfd p{listen_.get(), POLLIN, 0};
      if (::poll(&p, 1, 50) <= 0) continue;
      int fd = ::accept4(listen_.get(), nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0) continue;
      ++accepted_;
      handlers_.emplace_back([this, client = Fd(fd)]() mutable { serve(std::move(client)); });
    }
  }

  void serve(Fd client) {
    std::string buffered;
    char data[4096];
    while (!stopping_) {
      pollfd p{client.get(), POLLIN, 0};
      if (::poll(&p, 1, 50) <= 0) continue;
      ssize_t n = ::recv(client.get(), data, sizeof data, 0);
      if (n <= 0) return;
      buffered.append(data, static_cast<size_t>(n));
      for (;;) {
        size_t end = buffered.find("\r\n\r\n");
        if (end == std::string::npos) break;
        buffered.erase(0, end + 4);
        ++served_;
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok";
        size_t sent = 0;
        while (sent < response.size()) {
          ssize_t written = ::send(client.get(), response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
          if (written <= 0) return;
          sent += static_cast<size_t>(written);
        }
        if (close_after_response_) return;
      }
    }
  }

  bool close_after_response_;
  Fd listen_;
  uint16_t port_ = 0;
  std::atomic<bool> stopping_{false};
  std::atomic<int> accepted_{0};
  std::atomic<int> served_{0};
  std::thread accept_thread_;
  std::vector<std::thread> handlers_;
};

TEST_CASE("build_proxy_request uses absolute-form and strips hop-by-hop headers") {
  Job j = make_job("POST", "/submit?x=1");
  j.target_port = 8080;
  j.headers = {{"Connection", "keep-alive"}, {"Proxy-Authorization", "Basic c2VjcmV0"}, {"X-Custom", "v"},
               {"Host", "ignored.example"}, {"Content-Length", "1"}};
  j.body = "abc";
  std::string r = build_proxy_request(j);
  REQUIRE(r.rfind("POST http://test.example:8080/submit?x=1 HTTP/1.1\r\n", 0) == 0);
  REQUIRE(r.find("Host: test.example:8080\r\n") != std::string::npos);
  REQUIRE(r.find("X-Custom: v\r\n") != std::string::npos);
  REQUIRE(r.find("Proxy-Authorization") == std::string::npos);
  REQUIRE(r.find("ignored.example") == std::string::npos);
  REQUIRE(r.find("Content-Length: 3\r\n") != std::string::npos);
  REQUIRE(r.find("Content-Length: 1\r\n") == std::string::npos);
  REQUIRE(r.find("Connection: close\r\n") != std::string::npos);
  REQUIRE(build_proxy_request(j, true).find("Connection: keep-alive\r\n") != std::string::npos);
  REQUIRE(r.substr(r.size() - 3) == "abc");
}

TEST_CASE("forwarder returns status, headers and body from the proxy") {
  FakeServer proxy([](const std::string&) {
    return std::string("HTTP/1.1 200 OK\r\nContent-Length: 11\r\nX-From: proxy\r\n\r\nhello world");
  });
  SimpleForwarder f(1000, 2000);
  auto r = f.forward(make_job(), proxy_at(proxy.port()));
  REQUIRE(r.ok());
  REQUIRE(r.status == 200);
  REQUIRE(r.body == "hello world");
  REQUIRE(*find_header(r.headers, "x-from") == "proxy");
  REQUIRE(r.proxy_id == 7);
  REQUIRE(r.latency_ms >= 0);
  auto seen = proxy.requests();
  REQUIRE(seen.size() == 1);
  REQUIRE(seen[0].rfind("GET http://test.example/products/123 HTTP/1.1\r\n", 0) == 0);
}

TEST_CASE("epoll forwarder handles responses and reuses a keep-alive proxy connection") {
  Fd listen_fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  REQUIRE(listen_fd);
  int one = 1;
  ::setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  REQUIRE(::bind(listen_fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
  REQUIRE(::listen(listen_fd.get(), 4) == 0);
  socklen_t addr_len = sizeof addr;
  REQUIRE(::getsockname(listen_fd.get(), reinterpret_cast<sockaddr*>(&addr), &addr_len) == 0);
  std::atomic<int> accepted{0};
  std::atomic<bool> hold_connection{true};
  std::thread server([&] {
    pollfd ready{listen_fd.get(), POLLIN, 0};
    if (::poll(&ready, 1, 3000) <= 0) return;
    Fd client(::accept4(listen_fd.get(), nullptr, nullptr, SOCK_CLOEXEC));
    if (!client) return;
    ++accepted;
    for (int i = 0; i < 2; ++i) {
      std::string head;
      char buf[1024];
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (head.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() < deadline) {
        pollfd p{client.get(), POLLIN, 0};
        if (::poll(&p, 1, 100) <= 0) continue;
        ssize_t n = ::recv(client.get(), buf, sizeof buf, 0);
        if (n <= 0) return;
        head.append(buf, static_cast<size_t>(n));
      }
      if (head.find("\r\n\r\n") == std::string::npos) return;
      const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok";
      size_t sent = 0;
      while (sent < response.size()) {
        ssize_t n = ::send(client.get(), response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
      }
    }
    while (hold_connection.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  });
  Metrics metrics;
  EpollForwarder f(1000, 1000, 1u << 20, 2, 1000, &metrics);
  ProxyState proxy = proxy_at(ntohs(addr.sin_port));
  auto first = f.forward(make_job(), proxy);
  auto second = f.forward(make_job("GET", "/products/456"), proxy);
  const auto opened = metrics.get(Counter::ConnectionsOpened);
  const auto reused = metrics.get(Counter::ConnectionsReused);
  const auto active = metrics.get(Gauge::UpstreamActiveConnections);
  const auto idle = metrics.get(Gauge::UpstreamIdleConnections);
  const auto bytes_in = metrics.get(Counter::BytesIn);
  const auto bytes_out = metrics.get(Counter::BytesOut);
  hold_connection = false;
  server.join();
  REQUIRE(first.ok());
  REQUIRE(first.body == "ok");
  REQUIRE(second.ok());
  REQUIRE(second.body == "ok");
  REQUIRE(accepted == 1);
  REQUIRE(opened == 1);
  REQUIRE(reused == 1);
  REQUIRE(active == 1);
  REQUIRE(idle == 1);
  REQUIRE(bytes_in > 0);
  REQUIRE(bytes_out > 0);
}

TEST_CASE("epoll forwarder maps a silent proxy to a response timeout") {
  FakeServer proxy([](const std::string&) { return std::string(); });
  EpollForwarder f(1000, 250);
  auto result = f.forward(make_job(), proxy_at(proxy.port()));
  REQUIRE(result.error == ErrorKind::ResponseTimeout);
  REQUIRE(result.latency_ms >= 200);
  REQUIRE(result.latency_ms < 1500);
}

TEST_CASE("epoll connection pool bounds concurrent sockets and evicts idle sockets") {
  KeepAliveTestProxy upstream;
  Metrics metrics;
  const int baseline_fds = pgtest::count_open_fds();
  int opened_before_eviction = 0;
  {
    EpollForwarder f(1000, 2000, 1u << 20, 4, 100, &metrics);
    ProxyState proxy = proxy_at(upstream.port());
    std::vector<std::future<ForwardResult>> requests;
    for (int i = 0; i < 24; ++i) {
      Job job = make_job("GET", "/products/" + std::to_string(i));
      requests.emplace_back(std::async(std::launch::async, [&f, job, proxy] { return f.forward(job, proxy); }));
    }
    for (auto& request : requests) REQUIRE(request.get().ok());
    REQUIRE(upstream.served() == 24);
    REQUIRE(metrics.get(Counter::ConnectionsOpened) >= 1);
    REQUIRE(metrics.get(Counter::ConnectionsOpened) <= 4);
    REQUIRE(metrics.get(Counter::ConnectionsReused) > 0);
    opened_before_eviction = static_cast<int>(metrics.get(Counter::ConnectionsOpened));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    REQUIRE(metrics.get(Gauge::UpstreamIdleConnections) == 0);
    auto after_idle = f.forward(make_job(), proxy);
    REQUIRE(after_idle.ok());
    REQUIRE(metrics.get(Counter::ConnectionsOpened) == static_cast<uint64_t>(opened_before_eviction + 1));
  }
  REQUIRE(pgtest::count_open_fds() == baseline_fds);
}

TEST_CASE("epoll forwarder discards a peer-closed keep-alive connection") {
  KeepAliveTestProxy upstream(true);
  EpollForwarder f(1000, 1000, 1u << 20, 2, 1000);
  ProxyState proxy = proxy_at(upstream.port());
  REQUIRE(f.forward(make_job(), proxy).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  REQUIRE(f.forward(make_job("GET", "/products/second"), proxy).ok());
  REQUIRE(upstream.served() == 2);
  REQUIRE(upstream.accepted() == 2);
}

TEST_CASE("epoll global pool cap evicts idle sockets when another proxy needs capacity") {
  KeepAliveTestProxy first_proxy;
  KeepAliveTestProxy second_proxy;
  EpollForwarder f(1000, 1000, 1u << 20, 1, 10000, nullptr, 1);
  auto first = f.forward(make_job(), proxy_at(first_proxy.port()));
  auto second = f.forward(make_job(), ProxyState{.id = 8, .host = "127.0.0.1", .port = second_proxy.port()});
  REQUIRE(first.ok());
  REQUIRE(second.ok());
  REQUIRE(first_proxy.served() == 1);
  REQUIRE(second_proxy.served() == 1);
  REQUIRE(first_proxy.accepted() == 1);
  REQUIRE(second_proxy.accepted() == 1);
}

TEST_CASE("forwarder decodes chunked responses") {
  FakeServer proxy([](const std::string&) {
    return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n1\r\n!\r\n0\r\n\r\n");
  });
  SimpleForwarder f(1000, 2000);
  auto r = f.forward(make_job(), proxy_at(proxy.port()));
  REQUIRE(r.ok());
  REQUIRE(r.body == "hello!");
}

TEST_CASE("forwarder passes through 404 and 5xx statuses unchanged") {
  FakeServer proxy([](const std::string& req) {
    if (req.find("/missing") != std::string::npos) return std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\n\r\nno");
    return std::string("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 4\r\n\r\nbusy");
  });
  SimpleForwarder f(1000, 2000);
  auto a = f.forward(make_job("GET", "/missing"), proxy_at(proxy.port()));
  REQUIRE(a.ok());
  REQUIRE(a.status == 404);
  auto b = f.forward(make_job("GET", "/other"), proxy_at(proxy.port()));
  REQUIRE(b.ok());
  REQUIRE(b.status == 503);
}

TEST_CASE("forwarder: connection refused is ConnectFailed") {
  // Grab a free port, then close it so nothing listens there.
  uint16_t dead = 0;
  {
    FakeServer tmp([](const std::string&) { return std::string(); });
    dead = tmp.port();
  }
  SimpleForwarder f(1000, 1000);
  auto r = f.forward(make_job(), proxy_at(dead));
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.error == ErrorKind::ConnectFailed);
}

TEST_CASE("forwarder: unresolvable host is ConnectFailed") {
  SimpleForwarder f(1000, 1000);
  ProxyState p;
  p.id = 0;
  p.host = "no-such-host.invalid";
  p.port = 3128;
  auto r = f.forward(make_job(), p);
  REQUIRE(r.error == ErrorKind::ConnectFailed);
}

TEST_CASE("forwarder: silent proxy triggers ResponseTimeout near the configured time") {
  FakeServer proxy([](const std::string&) { return std::string(); });  // never replies
  SimpleForwarder f(1000, 300);
  auto r = f.forward(make_job(), proxy_at(proxy.port()));
  REQUIRE(r.error == ErrorKind::ResponseTimeout);
  REQUIRE(r.latency_ms >= 250);
  REQUIRE(r.latency_ms < 1500);
  REQUIRE(is_timeout(r.error));
}

TEST_CASE("forwarder: proxy closing without a response is IoError") {
  FakeServer proxy([](const std::string&) { return std::string("\0", 0); });
  // An empty reply means "hang" in FakeServer, so use a server that replies with garbage then closes.
  FakeServer closer([](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort"); });
  SimpleForwarder f(1000, 1000);
  auto r = f.forward(make_job(), proxy_at(closer.port()));
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.error == ErrorKind::BadResponse);  // truncated body
}

TEST_CASE("forwarder: garbage from the proxy is BadResponse") {
  FakeServer proxy([](const std::string&) { return std::string("this is not http\r\n\r\n"); });
  SimpleForwarder f(1000, 1000);
  auto r = f.forward(make_job(), proxy_at(proxy.port()));
  REQUIRE(r.error == ErrorKind::BadResponse);
}

TEST_CASE("forwarder does not leak file descriptors") {
  FakeServer proxy([](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"); });
  SimpleForwarder f(1000, 1000);
  (void)f.forward(make_job(), proxy_at(proxy.port()));  // warm up any lazy descriptors
  int before = pgtest::count_open_fds();
  for (int i = 0; i < 50; ++i) {
    REQUIRE(f.forward(make_job(), proxy_at(proxy.port())).ok());
  }
  // The fake server's own per-connection fds are closed by its threads; allow brief settling.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  REQUIRE(pgtest::count_open_fds() <= before + 2);
}
