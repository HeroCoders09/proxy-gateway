#include <algorithm>
#include <deque>

#include "catch_amalgamated.hpp"
#include "admin/admin_server.hpp"

#include "gateway/gateway_core.hpp"
#include "routing/policy_router.hpp"

using namespace pg;

namespace {
std::vector<ProxyState> sample_proxies() {
  ProxyState a; a.id = 0; a.rtt_ms = 10; a.active_connections = 3; a.success_count = 8; a.failure_count = 2;
  ProxyState b; b.id = 1; b.rtt_ms = 50; b.active_connections = 1; b.success_count = 9; b.failure_count = 1;
  ProxyState c; c.id = 2; c.rtt_ms = 20; c.active_connections = 2; c.healthy = false;
  return {a, b, c};
}

GatewayConfig config_for(const std::string& policy = "roundrobin", int count = 3) {
  GatewayConfig cfg;
  cfg.policy = policy;
  cfg.proxies.clear();
  for (int i = 0; i < count; ++i) {
    ProxyEndpoint endpoint;
    endpoint.id = i;
    endpoint.host = "127.0.0.1";
    endpoint.port = static_cast<uint16_t>(3128 + i);
    cfg.proxies.push_back(std::move(endpoint));
  }
  return cfg;
}

struct SequenceForwarder final : Forwarder {
  std::deque<ForwardResult> results;
  std::vector<ProxyId> used;
  std::vector<uint16_t> ports;
  ForwardResult forward(const Job&, const ProxyState& proxy) override {
    used.push_back(proxy.id);
    ports.push_back(proxy.port);
    if (results.empty()) return ForwardResult{ErrorKind::None, {}, 200, {}, "ok", 12.0, proxy.id};
    auto result = results.front();
    results.pop_front();
    result.proxy_id = proxy.id;
    return result;
  }
};
}  // namespace

TEST_CASE("routing policies select by their documented rule") {
  Job job;
  const auto table = sample_proxies();
  SECTION("round robin cycles through eligible proxies") {
    PolicyRouter router("roundrobin", {});
    REQUIRE(router.select(job, table) == 0);
    REQUIRE(router.select(job, table) == 1);
    REQUIRE(router.select(job, table) == 0);
  }
  SECTION("least connections chooses the lowest count") {
    PolicyRouter router("leastconn", {});
    REQUIRE(router.select(job, table) == 1);
  }
  SECTION("latency chooses the lowest measured RTT") {
    PolicyRouter router("latency", {});
    REQUIRE(router.select(job, table) == 0);
  }
  SECTION("weighted score uses configured normalized dimensions") {
    WeightsCfg weights; weights.latency = 1; weights.load = 0; weights.failure = 0;
    PolicyRouter router("weighted", weights);
    REQUIRE(router.select(job, table) == 0);
  }
  SECTION("random always chooses an eligible proxy") {
    PolicyRouter router("random", {});
    const auto selected = router.select(job, table);
    REQUIRE(selected.has_value());
    REQUIRE((*selected == 0 || *selected == 1));
  }
}

TEST_CASE("router excludes unhealthy, OPEN and already tried proxies") {
  PolicyRouter router("roundrobin", {});
  Job job;
  auto table = sample_proxies();
  table[0].circuit_state = CircuitState::Open;
  job.tried_proxies.push_back(1);
  REQUIRE_FALSE(router.select(job, table).has_value());
}

TEST_CASE("latency policy samples each eligible proxy with no RTT measurement") {
  PolicyRouter router("latency", {});
  Job job;
  std::vector<ProxyState> table(3);
  for (int i = 0; i < 3; ++i) table[static_cast<size_t>(i)].id = i;
  REQUIRE(router.select(job, table) == 0);
  REQUIRE(router.select(job, table) == 1);
  REQUIRE(router.select(job, table) == 2);
}

TEST_CASE("least-connection selections remain balanced as reservations are recorded") {
  PolicyRouter router("leastconn", {});
  Job job;
  std::vector<ProxyState> table(3);
  for (int i = 0; i < 3; ++i) table[static_cast<size_t>(i)].id = i;
  for (int request = 0; request < 30; ++request) {
    const auto selected = router.select(job, table);
    REQUIRE(selected.has_value());
    ++table[static_cast<size_t>(*selected)].active_connections;
  }
  const auto [minimum, maximum] = std::minmax_element(table.begin(), table.end(), [](const auto& a, const auto& b) {
    return a.active_connections < b.active_connections;
  });
  REQUIRE(maximum->active_connections - minimum->active_connections <= 1);
}

TEST_CASE("gateway retries transport failures through a different proxy, at most twice") {
  auto cfg = config_for("roundrobin", 2);
  cfg.proxies[0].port = 3130;  // Proxy 3 in the test bed.
  cfg.proxies[1].port = 3128;
  Metrics metrics;
  SequenceForwarder forwarder;
  forwarder.results.push_back({ErrorKind::ResponseTimeout, "timed out", 0, {}, {}, 10.0, -1});
  GatewayCore core(cfg, forwarder, metrics);
  Job job;
  HttpResponse response = core.handle(job);
  REQUIRE(response.status == 200);
  REQUIRE(forwarder.used == std::vector<ProxyId>{0, 1});
  REQUIRE(forwarder.ports == std::vector<uint16_t>{3130, 3128});
  REQUIRE(job.retry_count == 1);
  REQUIRE(metrics.get(Counter::Retries) == 1);
  REQUIRE(metrics.get(Counter::Timeouts) == 1);
  const auto states = core.proxies();
  REQUIRE(states[0].active_connections == 0);
  REQUIRE(states[1].active_connections == 0);
  REQUIRE(states[0].failure_count == 1);
  REQUIRE(states[1].success_count == 1);
  REQUIRE(states[1].rtt_ms == 12.0);
}

TEST_CASE("gateway stops after two retries and releases all active reservations") {
  auto cfg = config_for();
  Metrics metrics;
  SequenceForwarder forwarder;
  for (int i = 0; i < 3; ++i)
    forwarder.results.push_back({ErrorKind::ResponseTimeout, "timeout", 0, {}, {}, 2.0, -1});
  GatewayCore core(cfg, forwarder, metrics);
  Job job;
  HttpResponse response = core.handle(job);
  REQUIRE(response.status == 504);
  REQUIRE(forwarder.used == std::vector<ProxyId>{0, 2, 1});
  REQUIRE(job.retry_count == 2);
  REQUIRE(metrics.get(Counter::Retries) == 2);
  REQUIRE(metrics.get(Counter::Timeouts) == 3);
  REQUIRE(metrics.get(Counter::RequestsFailed) == 1);
  for (const auto& proxy : core.proxies()) {
    REQUIRE(proxy.active_connections == 0);
    REQUIRE(proxy.failure_count == 1);
  }
}

TEST_CASE("gateway returns 502 and counts an empty route") {
  auto cfg = config_for("roundrobin", 1);
  Metrics metrics;
  SequenceForwarder forwarder;
  GatewayCore core(cfg, forwarder, metrics);
  Job job;
  job.tried_proxies.push_back(0);
  REQUIRE(core.handle(job).status == 502);
  REQUIRE(metrics.get(Counter::NoRoute) == 1);
  REQUIRE(metrics.get(Counter::RequestsFailed) == 1);
  REQUIRE(forwarder.used.empty());
}

TEST_CASE("admin exposes live Phase 3 proxy and routing metrics") {
  auto cfg = config_for();
  Metrics metrics;
  metrics.inc(Counter::Retries, 2);
  metrics.inc(Counter::NoRoute);
  metrics.inc(Counter::LeastConnSelections, 3);
  auto states = sample_proxies();
  AdminServer admin(cfg, metrics, {}, [states] { return states; });
  const auto proxies = admin.route("GET", "/proxies");
  REQUIRE(proxies.status == 200);
  REQUIRE(proxies.body.find("\"requests_sent\":10") != std::string::npos);
  REQUIRE(proxies.body.find("\"active_connections\":3") != std::string::npos);
  const auto stats = admin.route("GET", "/api/stats");
  REQUIRE(stats.body.find("\"retries\":2") != std::string::npos);
  REQUIRE(stats.body.find("\"no_route\":1") != std::string::npos);
  REQUIRE(stats.body.find("\"leastconn\":3") != std::string::npos);
  const auto metric_text = admin.route("GET", "/metrics");
  REQUIRE(metric_text.body.find("proxy_requests_total{proxy=\"0\"} 10") != std::string::npos);
  REQUIRE(metric_text.body.find("proxy_active_connections{proxy=\"0\"} 3") != std::string::npos);
}
