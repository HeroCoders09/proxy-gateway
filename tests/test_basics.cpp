#include <thread>

#include "catch_amalgamated.hpp"
#include "common/clock.hpp"
#include "common/fd.hpp"
#include "common/http_util.hpp"
#include "common/metrics.hpp"
#include "test_util.hpp"

using namespace pg;

TEST_CASE("fd wrapper closes on destruction and supports move") {
  int before = pgtest::count_open_fds();
  {
    int p[2];
    REQUIRE(::pipe(p) == 0);
    Fd a(p[0]);
    Fd b(p[1]);
    REQUIRE(pgtest::count_open_fds() == before + 2);
    Fd c = std::move(a);
    REQUIRE_FALSE(a.valid());
    REQUIRE(c.valid());
    b.reset();  // explicit close
    REQUIRE(pgtest::count_open_fds() == before + 1);
  }
  REQUIRE(pgtest::count_open_fds() == before);
}

TEST_CASE("fd release gives up ownership") {
  int p[2];
  REQUIRE(::pipe(p) == 0);
  Fd a(p[0]);
  int raw = a.release();
  REQUIRE(raw == p[0]);
  REQUIRE_FALSE(a.valid());
  ::close(raw);
  ::close(p[1]);
}

TEST_CASE("fake clock only moves when advanced") {
  FakeClock c;
  auto t0 = c.now();
  REQUIRE(c.now() == t0);
  c.advance(std::chrono::seconds(3));
  REQUIRE(c.now() - t0 == std::chrono::seconds(3));
}

TEST_CASE("http_util basics") {
  REQUIRE(to_lower("AbC-X") == "abc-x");
  REQUIRE(iequals("Content-LENGTH", "content-length"));
  REQUIRE_FALSE(iequals("a", "ab"));
  REQUIRE(trim_ows(" \t x y \t") == "x y");
  REQUIRE(is_token("Content-Type"));
  REQUIRE_FALSE(is_token("Bad Name"));
  unsigned long long v = 0;
  REQUIRE(parse_uint("123", 1000, v));
  REQUIRE(v == 123);
  REQUIRE_FALSE(parse_uint("1001", 1000, v));
  REQUIRE_FALSE(parse_uint("-1", 1000, v));
  REQUIRE_FALSE(parse_uint("", 1000, v));
  REQUIRE_FALSE(parse_uint("99999999999999999999999", ~0ULL, v));
}

TEST_CASE("strip_hop_by_hop removes connection-scoped headers only") {
  Headers h{{"Connection", "keep-alive"}, {"Keep-Alive", "5"}, {"X-Test", "1"}, {"Transfer-Encoding", "chunked"},
            {"proxy-connection", "x"}, {"Content-Type", "text/plain"}};
  strip_hop_by_hop(h);
  REQUIRE(h.size() == 2);
  REQUIRE(find_header(h, "x-test") != nullptr);
  REQUIRE(find_header(h, "content-type") != nullptr);
}

TEST_CASE("metrics counters and percentiles") {
  Metrics m;
  m.inc(Counter::RequestsTotal);
  m.inc(Counter::RequestsTotal, 4);
  REQUIRE(m.get(Counter::RequestsTotal) == 5);
  REQUIRE(m.get(Counter::Timeouts) == 0);

  for (int i = 1; i <= 100; ++i) m.observe_latency_ms(i);
  auto p = m.latency_percentiles();
  REQUIRE(p.samples == 100);
  REQUIRE(p.p50 == 50);
  REQUIRE(p.p95 == 95);
  REQUIRE(p.p99 == 99);
}

TEST_CASE("metrics latency window is bounded") {
  Metrics m;
  for (size_t i = 0; i < Metrics::kLatencyRing * 3; ++i) m.observe_latency_ms(1.0);
  REQUIRE(m.latency_percentiles().samples == Metrics::kLatencyRing);
}

TEST_CASE("metrics empty percentiles are zero") {
  Metrics m;
  auto p = m.latency_percentiles();
  REQUIRE(p.samples == 0);
  REQUIRE(p.p99 == 0.0);
}

TEST_CASE("metrics are thread safe") {
  Metrics m;
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t) {
    ts.emplace_back([&m] {
      for (int i = 0; i < 5000; ++i) {
        m.inc(Counter::RequestsSuccessful);
        m.observe_latency_ms(i % 50);
      }
    });
  }
  for (auto& t : ts) t.join();
  REQUIRE(m.get(Counter::RequestsSuccessful) == 40000);
  const auto p = m.latency_percentiles();
  REQUIRE(p.samples == Metrics::kLatencyRing);
  REQUIRE(p.p95 <= 49.0);
}
