#include "catch_amalgamated.hpp"

#include <deque>

#include "gateway/gateway_core.hpp"
#include "health/health_state.hpp"
#include "routing/policy_router.hpp"

using namespace pg;

namespace {
struct HealthForwarder final : Forwarder {
  std::deque<ForwardResult> results;
  int calls = 0;
  ForwardResult forward(const Job& job, const ProxyState& proxy) override {
    ++calls;
    if (job.target_path != "/health") return {ErrorKind::None, {}, 200, {}, "ok", 1.0, proxy.id};
    if (results.empty()) return {ErrorKind::None, {}, 200, {}, "ok", 1.0, proxy.id};
    auto result = results.front();
    results.pop_front();
    result.proxy_id = proxy.id;
    return result;
  }
};

GatewayConfig health_config() {
  GatewayConfig cfg;
  cfg.proxies = {{0, "127.0.0.1", 3128}};
  cfg.circuit_threshold = 2;
  cfg.circuit_cooldown_ms = 10000;
  cfg.health_window_size = 4;
  return cfg;
}
}  // namespace

TEST_CASE("rolling health statistics retain only the configured recent window") {
  RollingStats stats(3);
  REQUIRE(stats.success_rate() == 1.0);
  stats.add(true, 10.0);
  stats.add(false, 20.0);
  stats.add(false, 30.0);
  REQUIRE(stats.size() == 3);
  REQUIRE(stats.success_rate() == Catch::Approx(1.0 / 3.0));
  REQUIRE(stats.mean_rtt_ms() == Catch::Approx(20.0));
  stats.add(true, 40.0);
  REQUIRE(stats.size() == 3);
  REQUIRE(stats.success_rate() == Catch::Approx(1.0 / 3.0));
  REQUIRE(stats.mean_rtt_ms() == Catch::Approx(30.0));
}

TEST_CASE("circuit opens at threshold, waits cooldown, and enforces one HALF_OPEN probe") {
  FakeClock clock;
  CircuitBreaker breaker(2, std::chrono::seconds(10), clock);
  breaker.record(false);
  REQUIRE(breaker.state() == CircuitState::Closed);
  breaker.record(false);
  REQUIRE(breaker.state() == CircuitState::Open);
  REQUIRE(breaker.open_count() == 1);
  REQUIRE_FALSE(breaker.begin_probe());
  clock.advance(std::chrono::seconds(10));
  REQUIRE(breaker.begin_probe());
  REQUIRE(breaker.state() == CircuitState::HalfOpen);
  REQUIRE_FALSE(breaker.begin_probe());
  breaker.finish_probe(false);
  REQUIRE(breaker.state() == CircuitState::Open);
  REQUIRE(breaker.open_count() == 2);
  clock.advance(std::chrono::seconds(10));
  REQUIRE(breaker.begin_probe());
  breaker.finish_probe(true);
  REQUIRE(breaker.state() == CircuitState::Closed);
  REQUIRE(breaker.consecutive_failures() == 0);
  REQUIRE(breaker.open_count() == 2);
}

TEST_CASE("health derives unhealthy state from rolling outcomes and recovers with good samples") {
  auto cfg = health_config();
  cfg.circuit_threshold = 20;
  Metrics metrics;
  HealthForwarder forwarder;
  FakeClock clock;
  GatewayCore core(cfg, forwarder, metrics, &clock);
  for (int i = 0; i < 4; ++i) core.report(0, false, 5.0);
  REQUIRE_FALSE(core.proxies()[0].healthy);
  REQUIRE(core.proxies()[0].rolling_success_rate == 0.0);
  core.report(0, true, 5.0);
  REQUIRE_FALSE(core.proxies()[0].healthy);
  core.report(0, true, 5.0);
  REQUIRE(core.proxies()[0].healthy);
  REQUIRE(core.proxies()[0].rolling_success_rate == Catch::Approx(0.5));
  core.report(0, true, 5.0);
  REQUIRE(core.proxies()[0].rolling_success_rate == Catch::Approx(0.75));
}

TEST_CASE("gateway uses HALF_OPEN only for its cooldown probe, then restores routing") {
  auto cfg = health_config();
  Metrics metrics;
  HealthForwarder forwarder;
  FakeClock clock;
  GatewayCore core(cfg, forwarder, metrics, &clock);
  core.report(0, false, 2.0);
  core.report(0, false, 2.0);
  REQUIRE(core.proxies()[0].circuit_state == CircuitState::Open);
  Job job;
  REQUIRE(core.handle(job).status == 502);
  REQUIRE(forwarder.calls == 0);
  REQUIRE_FALSE(core.probe_proxy(0));
  clock.advance(std::chrono::seconds(10));
  forwarder.results.push_back({ErrorKind::None, {}, 200, {}, "healthy", 3.0, 0});
  REQUIRE(core.probe_proxy(0));
  const auto state = core.proxies()[0];
  REQUIRE(state.circuit_state == CircuitState::Closed);
  REQUIRE(state.consecutive_failures == 0);
  REQUIRE(state.circuit_open_count == 1);
  REQUIRE(state.last_probe_unix_ms > 0);
  REQUIRE(metrics.get(Counter::ProbeSuccesses) == 1);
}

TEST_CASE("router excludes HALF_OPEN proxies from normal requests") {
  PolicyRouter router("roundrobin", {});
  ProxyState proxy;
  proxy.id = 0;
  proxy.circuit_state = CircuitState::HalfOpen;
  REQUIRE_FALSE(router.select(Job{}, {proxy}).has_value());
}
