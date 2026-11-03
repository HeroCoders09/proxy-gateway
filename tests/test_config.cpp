#include <fstream>

#include "catch_amalgamated.hpp"
#include "common/config.hpp"

using namespace pg;

static const char* kMinimal = R"({"proxies":[{"host":"p1","port":3128}]})";

static std::string expect_error(const std::string& json) {
  try {
    parse_config_text(json);
  } catch (const ConfigError& e) {
    return e.what();
  }
  return "<no error>";
}

TEST_CASE("minimal config gets defaults") {
  GatewayConfig c = parse_config_text(kMinimal);
  REQUIRE(c.listen_port == 8000);
  REQUIRE(c.admin_port == 9000);
  REQUIRE(c.admin_host == "127.0.0.1");
  REQUIRE(c.policy == "roundrobin");
  REQUIRE(c.queue_limit == 10000);
  REQUIRE(c.connect_timeout_ms == 2000);
  REQUIRE(c.response_timeout_ms == 10000);
  REQUIRE(c.circuit_threshold == 5);
  REQUIRE(c.circuit_cooldown_ms == 10000);
  REQUIRE(c.health_probe_interval_ms == 2000);
  REQUIRE(c.health_window_size == 20);
  REQUIRE(c.proxies.size() == 1);
  REQUIRE(c.proxies[0].id == 0);
}

TEST_CASE("config reads upstream authentication without embedding a password") {
  auto c = parse_config_text(R"({"proxies":[{"host":"provider","port":3128,"auth":{"username":"acct","password_env":"UPSTREAM_PASSWORD"}}]})");
  REQUIRE(c.proxies[0].auth_username == "acct");
  REQUIRE(c.proxies[0].auth_password_env == "UPSTREAM_PASSWORD");
  REQUIRE(c.proxies[0].auth_password.empty());
  REQUIRE(expect_error(R"({"proxies":[{"host":"provider","port":3128,"auth":{"username":"acct"}}]})").find("auth requires") != std::string::npos);
}

TEST_CASE("shipped example config is valid") {
  GatewayConfig c = load_config_file(PG_SOURCE_DIR "/config/gateway.json");
  REQUIRE(c.proxies.size() == 3);
  REQUIRE(c.proxies[2].id == 2);
  REQUIRE(c.domain_rates.at("site-a.example").rate == 50);
  REQUIRE(c.default_rate.rate == 1000);
}

TEST_CASE("full config round trip of values") {
  GatewayConfig c = parse_config_text(R"({
    "listen":{"host":"127.0.0.1","port":8123},
    "admin":{"port":9123},
    "proxies":[{"host":"a","port":1},{"host":"b","port":2}],
    "policy":"weighted","forwarder":"simple",
    "forwarder_pool":{"max_connections_per_proxy":12,"max_total_connections":40,"idle_timeout_ms":25000},
    "rate_limits":{"default":{"rate":10,"burst":20},"Site-A.Example":{"rate":5}},
    "queue_limit":77,
    "workers":{"min":2,"max":9,"alpha":2,"beta":0.7},
    "timeouts":{"connect_ms":111,"response_ms":222},
    "circuit":{"threshold":3,"cooldown_ms":444,"probe_interval_ms":1234,"window_size":7},
    "weights":{"latency":1,"load":2,"failure":3},
    "ingress":{"threads":3},
    "output":{"format":"csv","path":"out.csv"}})");
  REQUIRE(c.listen_port == 8123);
  REQUIRE(c.admin_port == 9123);
  REQUIRE(c.policy == "weighted");
  REQUIRE(c.forwarder_pool.max_connections_per_proxy == 12);
  REQUIRE(c.forwarder_pool.max_total_connections == 40);
  REQUIRE(c.forwarder_pool.idle_timeout_ms == 25000);
  REQUIRE(c.queue_limit == 77);
  REQUIRE(c.workers.max == 9);
  REQUIRE(c.workers.beta == Catch::Approx(0.7));
  REQUIRE(c.connect_timeout_ms == 111);
  REQUIRE(c.circuit_threshold == 3);
  REQUIRE(c.health_probe_interval_ms == 1234);
  REQUIRE(c.health_window_size == 7);
  REQUIRE(c.weights.failure == 3);
  REQUIRE(c.ingress.threads == 3);
  // Domain keys are lower-cased; unspecified burst inherits the default.
  REQUIRE(c.domain_rates.at("site-a.example").rate == 5);
  REQUIRE(c.domain_rates.at("site-a.example").burst == 20);
  REQUIRE(c.output.format == "csv");
}

TEST_CASE("invalid configs are rejected with a clear message") {
  REQUIRE(expect_error("{not json").find("invalid JSON") != std::string::npos);
  REQUIRE(expect_error("[]").find("must be an object") != std::string::npos);
  REQUIRE(expect_error("{}").find("proxies must contain") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[]})").find("proxies must contain") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p"}]})").find("proxies[0].port is required") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":70000}]})").find("between 1 and 65535") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":"80"}]})").find("must be an integer") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"","port":80}]})").find("host must not be empty") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"polcy":"x"})").find("unknown key 'polcy'") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"policy":"fastest"})").find("policy must be one of") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"forwarder":"magic"})").find("forwarder must be") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"forwarder_pool":{"max_connections_per_proxy":10,"max_total_connections":5}})").find("must not exceed") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"workers":{"beta":1.5}})").find("workers.beta") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"workers":{"beta":0}})").find("workers.beta") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"workers":{"min":10,"max":5}})").find("workers.max") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"workers":{"alpha":0}})").find("workers.alpha") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"timeouts":{"connect_ms":0}})").find("timeouts.connect_ms") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"rate_limits":{"a.com":{"rate":-1}}})").find("rate") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"output":{"format":"csv"}})").find("output.path") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"listen":{"port":0}})").find("listen.port") != std::string::npos);
  REQUIRE(expect_error(R"({"proxies":[{"host":"p","port":80}],"circuit":{"threshold":0}})").find("circuit.threshold") != std::string::npos);
}

TEST_CASE("missing config file is a ConfigError") {
  REQUIRE_THROWS_AS(load_config_file("/nonexistent/gateway.json"), ConfigError);
}

TEST_CASE("CLI parsing") {
  auto parse = [](std::vector<const char*> v) {
    v.insert(v.begin(), "gateway");
    return parse_cli(static_cast<int>(v.size()), v.data());
  };
  REQUIRE(parse({"--help"}).help);
  REQUIRE(parse({"-h"}).help);
  auto o = parse({"--config", "g.json", "--policy", "latency", "--workers", "20"});
  REQUIRE(o.config_path == "g.json");
  REQUIRE(*o.policy == "latency");
  REQUIRE(*o.workers == 20);
  auto o2 = parse({"--config=x.json", "--policy=random"});
  REQUIRE(o2.config_path == "x.json");
  REQUIRE(*o2.policy == "random");

  REQUIRE_THROWS_AS(parse({"--bogus"}), ConfigError);
  REQUIRE_THROWS_AS(parse({"--config"}), ConfigError);
  REQUIRE_THROWS_AS(parse({"--policy", "fastest"}), ConfigError);
  REQUIRE_THROWS_AS(parse({"--workers", "abc"}), ConfigError);
  REQUIRE_THROWS_AS(parse({"--workers", "0"}), ConfigError);
  REQUIRE_THROWS_AS(parse({"--workers", "-3"}), ConfigError);
  REQUIRE_THROWS_AS(parse({"positional"}), ConfigError);
}

TEST_CASE("CLI overrides are applied and re-validated") {
  GatewayConfig c = parse_config_text(kMinimal);
  CliOptions o;
  o.policy = "leastconn";
  o.workers = 20;
  apply_cli_overrides(c, o);
  REQUIRE(c.policy == "leastconn");
  REQUIRE(c.workers.max == 20);

  CliOptions bad;
  bad.workers = 1;  // below workers.min (4)
  REQUIRE_THROWS_AS(apply_cli_overrides(c, bad), ConfigError);
}

TEST_CASE("usage text mentions every option") {
  auto u = usage_text();
  for (const char* opt : {"--config", "--policy", "--workers", "--help"}) {
    REQUIRE(u.find(opt) != std::string::npos);
  }
}
