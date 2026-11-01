#include "catch_amalgamated.hpp"
#include "common/config.hpp"
#include "gateway/gateway_core.hpp"
#include "ingress/listener.hpp"
#include "net/simple_forwarder.hpp"
#include "test_util.hpp"

using namespace pg;
using pgtest::FakeServer;
using pgtest::roundtrip;
using pgtest::status_of;

static GatewayConfig test_config(uint16_t proxy_port) {
  GatewayConfig c = parse_config_text(R"({"proxies":[{"host":"127.0.0.1","port":1}],
      "listen":{"host":"127.0.0.1","port":1},"ingress":{"threads":4,"read_timeout_ms":600}})");
  c.listen_port = 0;  // ephemeral
  c.proxies[0].port = proxy_port;
  c.connect_timeout_ms = 500;
  c.response_timeout_ms = 500;
  return c;
}

TEST_CASE("end to end: client -> gateway -> fake proxy -> client") {
  FakeServer proxy([](const std::string&) {
    return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: keep-alive\r\n\r\nproduct-1");
  });
  GatewayConfig cfg = test_config(proxy.port());
  Metrics metrics;
  SimpleForwarder fwd(cfg.connect_timeout_ms, cfg.response_timeout_ms);
  GatewayCore core(cfg, fwd, metrics);
  Listener listener(cfg, metrics, [&](Job& j) { return core.handle(j); });
  listener.start();

  std::string resp = roundtrip(listener.port(), "GET http://test.example/products/1 HTTP/1.1\r\nHost: test.example\r\n\r\n");
  REQUIRE(status_of(resp) == 200);
  REQUIRE(resp.find("product-1") != std::string::npos);
  REQUIRE(resp.find("X-Gateway-Proxy: 0") != std::string::npos);
  REQUIRE(resp.find("Connection: close") != std::string::npos);
  REQUIRE(metrics.get(Counter::RequestsTotal) == 1);
  REQUIRE(metrics.get(Counter::RequestsSuccessful) == 1);
  REQUIRE(metrics.get(Counter::RequestsFailed) == 0);

  auto seen = proxy.requests();
  REQUIRE(seen.size() == 1);
  REQUIRE(seen[0].rfind("GET http://test.example/products/1 HTTP/1.1", 0) == 0);
}

TEST_CASE("malformed and garbage requests get 400 and the gateway keeps running") {
  FakeServer proxy([](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"); });
  GatewayConfig cfg = test_config(proxy.port());
  Metrics metrics;
  SimpleForwarder fwd(cfg.connect_timeout_ms, cfg.response_timeout_ms);
  GatewayCore core(cfg, fwd, metrics);
  Listener listener(cfg, metrics, [&](Job& j) { return core.handle(j); });
  listener.start();
  const uint16_t port = listener.port();

  REQUIRE(status_of(roundtrip(port, "THIS IS NOT HTTP\r\n\r\n")) == 400);
  REQUIRE(status_of(roundtrip(port, "GET /relative HTTP/1.1\r\n\r\n")) == 400);
  REQUIRE(status_of(roundtrip(port, "GET https://secure.example/ HTTP/1.1\r\n\r\n")) == 400);
  REQUIRE(status_of(roundtrip(port, std::string("\x00\xff\xfe\x01 binary junk\r\n\r\n", 20))) == 400);
  REQUIRE(status_of(roundtrip(port, "GET http://h/ HTTP/1.1\r\nBad Header\r\n\r\n")) == 400);
  REQUIRE(status_of(roundtrip(port, "GET http://h/ HTTP/1.1\r\nX: " + std::string(40000, 'a') + "\r\n\r\n")) == 400);
  // Incomplete request that never finishes: times out with 400.
  REQUIRE(status_of(roundtrip(port, "GET http://h/ HTTP/1.1\r\nHost: h\r\n")) == 400);
  REQUIRE(metrics.get(Counter::BadRequests) >= 7);

  // Peer resets / silent connections must not hurt.
  for (int i = 0; i < 20; ++i) {
    Fd c = pgtest::connect_local(port);
    REQUIRE(c.valid());
    ::send(c.get(), "GET http://h/ HT", 16, MSG_NOSIGNAL);
    linger lg{1, 0};  // RST on close
    ::setsockopt(c.get(), SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
  }
  { Fd c = pgtest::connect_local(port); }  // connect and close immediately

  // Still serving valid requests.
  std::string ok = roundtrip(port, "GET http://test.example/products/2 HTTP/1.1\r\n\r\n");
  REQUIRE(status_of(ok) == 200);
  REQUIRE(ok.find("ok") != std::string::npos);
}

TEST_CASE("proxy failures map to 502 and 504 and update counters") {
  // Silent proxy -> timeout -> 504.
  FakeServer silent([](const std::string&) { return std::string(); });
  {
    GatewayConfig cfg = test_config(silent.port());
    Metrics metrics;
    SimpleForwarder fwd(cfg.connect_timeout_ms, cfg.response_timeout_ms);
    GatewayCore core(cfg, fwd, metrics);
    Listener listener(cfg, metrics, [&](Job& j) { return core.handle(j); });
    listener.start();
    REQUIRE(status_of(roundtrip(listener.port(), "GET http://t.example/ HTTP/1.1\r\n\r\n", 4000)) == 504);
    REQUIRE(metrics.get(Counter::Timeouts) == 1);
    REQUIRE(metrics.get(Counter::RequestsFailed) == 1);
  }
  // Dead proxy port -> connection refused -> 502.
  uint16_t dead;
  { FakeServer tmp([](const std::string&) { return std::string(); }); dead = tmp.port(); }
  {
    GatewayConfig cfg = test_config(dead);
    Metrics metrics;
    SimpleForwarder fwd(cfg.connect_timeout_ms, cfg.response_timeout_ms);
    GatewayCore core(cfg, fwd, metrics);
    Listener listener(cfg, metrics, [&](Job& j) { return core.handle(j); });
    listener.start();
    REQUIRE(status_of(roundtrip(listener.port(), "GET http://t.example/ HTTP/1.1\r\n\r\n")) == 502);
    REQUIRE(metrics.get(Counter::RequestsFailed) == 1);
    REQUIRE(metrics.get(Counter::Timeouts) == 0);
  }
}

TEST_CASE("proxy 5xx counts as failed but is passed through; 404 counts as successful") {
  FakeServer proxy([](const std::string& req) {
    if (req.find("/gone") != std::string::npos) return std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    return std::string("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
  });
  GatewayConfig cfg = test_config(proxy.port());
  Metrics metrics;
  SimpleForwarder fwd(cfg.connect_timeout_ms, cfg.response_timeout_ms);
  GatewayCore core(cfg, fwd, metrics);
  Listener listener(cfg, metrics, [&](Job& j) { return core.handle(j); });
  listener.start();
  REQUIRE(status_of(roundtrip(listener.port(), "GET http://t.example/gone HTTP/1.1\r\n\r\n")) == 404);
  REQUIRE(status_of(roundtrip(listener.port(), "GET http://t.example/x HTTP/1.1\r\n\r\n")) == 503);
  REQUIRE(metrics.get(Counter::RequestsSuccessful) == 1);
  REQUIRE(metrics.get(Counter::RequestsFailed) == 1);
}

TEST_CASE("HEAD responses carry no body") {
  FakeServer proxy([](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n"); });
  GatewayConfig cfg = test_config(proxy.port());
  Metrics metrics;
  SimpleForwarder fwd(cfg.connect_timeout_ms, cfg.response_timeout_ms);
  GatewayCore core(cfg, fwd, metrics);
  Listener listener(cfg, metrics, [&](Job& j) { return core.handle(j); });
  listener.start();
  std::string r = roundtrip(listener.port(), "HEAD http://t.example/ HTTP/1.1\r\n\r\n");
  REQUIRE(status_of(r) == 200);
  REQUIRE(r.find("Content-Length: 1234") != std::string::npos);
  REQUIRE(r.substr(r.size() - 4) == "\r\n\r\n");
}

TEST_CASE("handler exceptions become 502 and do not kill the listener") {
  GatewayConfig cfg = test_config(1);
  Metrics metrics;
  int calls = 0;
  Listener listener(cfg, metrics, [&](Job&) -> HttpResponse {
    if (calls++ == 0) throw std::runtime_error("boom");
    HttpResponse r;
    r.body = "fine";
    return r;
  });
  listener.start();
  REQUIRE(status_of(roundtrip(listener.port(), "GET http://t.example/ HTTP/1.1\r\n\r\n")) == 502);
  REQUIRE(status_of(roundtrip(listener.port(), "GET http://t.example/ HTTP/1.1\r\n\r\n")) == 200);
}

TEST_CASE("many sequential requests succeed with correct bodies and no fd leak") {
  FakeServer proxy([](const std::string& req) {
    // Echo the path as the body so each response is checkable.
    size_t a = req.find("/products/");
    size_t b = req.find(' ', a);
    std::string path = req.substr(a, b - a);
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(path.size()) + "\r\n\r\n" + path;
  });
  GatewayConfig cfg = test_config(proxy.port());
  Metrics metrics;
  SimpleForwarder fwd(cfg.connect_timeout_ms, cfg.response_timeout_ms);
  GatewayCore core(cfg, fwd, metrics);
  Listener listener(cfg, metrics, [&](Job& j) { return core.handle(j); });
  listener.start();
  (void)roundtrip(listener.port(), "GET http://t.example/products/0 HTTP/1.1\r\n\r\n");
  int before = pgtest::count_open_fds();
  for (int i = 1; i <= 300; ++i) {
    std::string resp = roundtrip(listener.port(), "GET http://t.example/products/" + std::to_string(i) + " HTTP/1.1\r\n\r\n");
    REQUIRE(status_of(resp) == 200);
    REQUIRE(resp.substr(resp.size() - std::to_string(i).size() - 10) == "/products/" + std::to_string(i));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  REQUIRE(pgtest::count_open_fds() <= before + 3);
  REQUIRE(metrics.get(Counter::RequestsTotal) == 301);
  REQUIRE(metrics.get(Counter::RequestsSuccessful) == 301);
}

TEST_CASE("listener uses a fixed number of threads under concurrent load") {
  FakeServer proxy([](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"); });
  GatewayConfig cfg = test_config(proxy.port());
  Metrics metrics;
  SimpleForwarder fwd(cfg.connect_timeout_ms, cfg.response_timeout_ms);
  GatewayCore core(cfg, fwd, metrics);
  Listener listener(cfg, metrics, [&](Job& j) { return core.handle(j); });
  listener.start();
  std::atomic<int> ok{0};
  std::vector<std::thread> clients;
  for (int t = 0; t < 8; ++t) {
    clients.emplace_back([&] {
      for (int i = 0; i < 25; ++i) {
        if (status_of(roundtrip(listener.port(), "GET http://t.example/ HTTP/1.1\r\n\r\n")) == 200) ++ok;
      }
    });
  }
  for (auto& c : clients) c.join();
  REQUIRE(ok == 200);
}

TEST_CASE("an oversized request gets its 400 and then a clean close, not a TCP reset (D-017)") {
  FakeServer proxy([](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"); });
  GatewayConfig cfg = test_config(proxy.port());
  Metrics metrics;
  SimpleForwarder fwd(cfg.connect_timeout_ms, cfg.response_timeout_ms);
  GatewayCore core(cfg, fwd, metrics);
  Listener listener(cfg, metrics, [&](Job& j) { return core.handle(j); });
  listener.start();

  // 60 KB of header data is well over max_header_bytes but fits in the socket buffers, so it is
  // all queued at the gateway when it answers. Closing right away would leave unread bytes and make
  // Linux send an RST after the 400. With the lingering close the client sees a normal EOF.
  int resets = 0;
  for (int i = 0; i < 20; ++i) {
    Fd c = pgtest::connect_local(listener.port());
    REQUIRE(c.valid());
    std::string big = "GET http://h/ HTTP/1.1\r\nX: " + std::string(60000, 'a') + "\r\n\r\n";
    REQUIRE(::send(c.get(), big.data(), big.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(big.size()));
    std::string got;
    char buf[4096];
    for (;;) {
      pollfd p{c.get(), POLLIN, 0};
      if (::poll(&p, 1, 3000) <= 0) break;
      ssize_t n = ::recv(c.get(), buf, sizeof buf, 0);
      if (n > 0) { got.append(buf, static_cast<size_t>(n)); continue; }
      if (n < 0 && errno == ECONNRESET) ++resets;
      break;  // 0 = clean EOF
    }
    REQUIRE(pgtest::status_of(got) == 400);
  }
  REQUIRE(resets == 0);
  REQUIRE(pgtest::status_of(roundtrip(listener.port(), "GET http://test.example/products/2 HTTP/1.1\r\n\r\n")) == 200);
}
