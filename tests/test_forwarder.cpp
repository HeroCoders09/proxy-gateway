#include "catch_amalgamated.hpp"
#include "common/http_util.hpp"
#include "net/simple_forwarder.hpp"
#include "test_util.hpp"

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
