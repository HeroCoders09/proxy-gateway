#include <nlohmann/json.hpp>

#include "admin/admin_server.hpp"
#include "catch_amalgamated.hpp"
#include "test_util.hpp"

using namespace pg;
using json = nlohmann::json;

static GatewayConfig cfg3() {
  GatewayConfig c = parse_config_text(R"({"proxies":[{"host":"proxy1","port":3128},{"host":"proxy2","port":3128},{"host":"proxy3","port":3128}]})");
  c.admin_port = 0;
  return c;
}

TEST_CASE("admin /health shape") {
  Metrics m;
  AdminServer a(cfg3(), m);
  auto r = a.route("GET", "/health");
  REQUIRE(r.status == 200);
  REQUIRE(r.content_type == "application/json");
  json j = json::parse(r.body);
  REQUIRE(j["status"] == "ok");
  REQUIRE(j["uptime_seconds"].is_number());
  REQUIRE(j["version"].is_string());
  REQUIRE(j["stub"] == true);
}

TEST_CASE("admin /proxies shape has one entry per configured proxy") {
  Metrics m;
  AdminServer a(cfg3(), m);
  json j = json::parse(a.route("GET", "/proxies").body);
  REQUIRE(j["proxies"].size() == 3);
  const json& p = j["proxies"][2];
  for (const char* key : {"id", "host", "port", "healthy", "rtt_ms", "success_count", "failure_count", "consecutive_failures",
                          "success_rate", "active_connections", "circuit_state", "circuit_open_count", "last_probe_unix_ms"}) {
    INFO(key);
    REQUIRE(p.contains(key));
  }
  REQUIRE(p["id"] == 2);
  REQUIRE(p["host"] == "proxy3");
  REQUIRE(p["circuit_state"] == "CLOSED");
}

TEST_CASE("admin /api/stats shape and real counters") {
  Metrics m;
  m.inc(Counter::RequestsTotal, 10);
  m.inc(Counter::RequestsSuccessful, 8);
  m.inc(Counter::RequestsFailed, 2);
  m.inc(Counter::Timeouts, 1);
  m.observe_latency_ms(12.5);
  AdminServer a(cfg3(), m);
  json j = json::parse(a.route("GET", "/api/stats").body);
  for (const char* key : {"timestamp_unix_ms", "policy", "requests_total", "requests_successful", "requests_failed", "timeouts",
                          "requests_per_second", "success_rate", "active_connections", "worker_count", "aimd_limit",
                          "queue_length", "latency_ms"}) {
    INFO(key);
    REQUIRE(j.contains(key));
  }
  REQUIRE(j["requests_total"] == 10);
  REQUIRE(j["success_rate"].get<double>() == Catch::Approx(0.8));
  REQUIRE(j["latency_ms"]["p50"].get<double>() == Catch::Approx(12.5));
  REQUIRE(j["latency_ms"].contains("p95"));
  REQUIRE(j["latency_ms"].contains("p99"));
}

TEST_CASE("admin /metrics is Prometheus text with the required names") {
  Metrics m;
  m.inc(Counter::RequestsTotal, 3);
  AdminServer a(cfg3(), m);
  auto r = a.route("GET", "/metrics");
  REQUIRE(r.status == 200);
  REQUIRE(r.content_type.find("text/plain") == 0);
  for (const char* name : {"requests_total 3", "requests_successful", "requests_failed", "timeouts", "active_connections",
                           "requests_per_second", "proxy_rtt{proxy=\"0\"}", "proxy_success_rate", "proxy_failure_count",
                           "worker_count", "queue_length", "latency_ms{quantile=\"0.99\"}"}) {
    INFO(name);
    REQUIRE(r.body.find(name) != std::string::npos);
  }
}

TEST_CASE("admin routing errors") {
  Metrics m;
  AdminServer a(cfg3(), m);
  REQUIRE(a.route("GET", "/nope").status == 404);
  REQUIRE(a.route("POST", "/health").status == 405);
  REQUIRE(a.route("GET", "/health?x=1").status == 200);
  REQUIRE(a.route("GET", "/ui").status == 200);
}

TEST_CASE("admin server binds to localhost and answers over a socket") {
  Metrics m;
  GatewayConfig c = cfg3();
  REQUIRE(c.admin_host == "127.0.0.1");
  AdminServer a(c, m);
  a.start();
  std::string resp = pgtest::roundtrip(a.port(), "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
  REQUIRE(pgtest::status_of(resp) == 200);
  REQUIRE(resp.find("\"status\":\"ok\"") != std::string::npos);
  REQUIRE(pgtest::status_of(pgtest::roundtrip(a.port(), "garbage\r\n\r\n")) == 400);
  a.stop();
}
