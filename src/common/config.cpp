#include "common/config.hpp"

#include <cmath>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "common/http_util.hpp"

namespace pg {

using json = nlohmann::json;

namespace {

[[noreturn]] void fail(const std::string& msg) { throw ConfigError("config: " + msg); }

void check_keys(const json& obj, const std::string& where, std::initializer_list<const char*> allowed) {
  if (!obj.is_object()) fail(where + " must be an object");
  std::set<std::string> ok(allowed.begin(), allowed.end());
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    if (!ok.count(it.key())) {
      fail("unknown key '" + (where.empty() ? "" : where + ".") + it.key() + "'");
    }
  }
}

std::string join(const std::string& where, const char* key) {
  return where.empty() ? std::string(key) : where + "." + key;
}

long long read_int(const json& o, const char* key, const std::string& where, long long def, long long lo,
                   long long hi) {
  auto it = o.find(key);
  if (it == o.end()) return def;
  if (!it->is_number_integer()) fail(join(where, key) + " must be an integer");
  long long v = it->get<long long>();
  if (v < lo || v > hi) {
    fail(join(where, key) + " must be between " + std::to_string(lo) + " and " + std::to_string(hi) +
         ", got " + std::to_string(v));
  }
  return v;
}

double read_double(const json& o, const char* key, const std::string& where, double def) {
  auto it = o.find(key);
  if (it == o.end()) return def;
  if (!it->is_number()) fail(join(where, key) + " must be a number");
  double v = it->get<double>();
  if (!std::isfinite(v)) fail(join(where, key) + " must be finite");
  return v;
}

std::string read_string(const json& o, const char* key, const std::string& where, const std::string& def) {
  auto it = o.find(key);
  if (it == o.end()) return def;
  if (!it->is_string()) fail(join(where, key) + " must be a string");
  return it->get<std::string>();
}

const json* read_object(const json& o, const char* key) {
  auto it = o.find(key);
  return it == o.end() ? nullptr : &*it;
}

RateLimitCfg read_rate(const json& o, const std::string& where, const RateLimitCfg& def) {
  check_keys(o, where, {"rate", "burst"});
  RateLimitCfg r = def;
  r.rate = read_double(o, "rate", where, def.rate);
  r.burst = read_double(o, "burst", where, def.burst);
  return r;
}

}  // namespace

bool is_valid_policy(const std::string& p) {
  return p == "random" || p == "roundrobin" || p == "leastconn" || p == "latency" || p == "weighted";
}

void validate_config(const GatewayConfig& c) {
  if (c.listen_port == 0) fail("listen.port must be 1-65535");
  if (c.admin_port == 0) fail("admin.port must be 1-65535");
  if (c.listen_host.empty()) fail("listen.host must not be empty");
  if (c.admin_host.empty()) fail("admin.host must not be empty");
  if (c.proxies.empty()) fail("proxies must contain at least one {host, port} entry");
  for (const auto& p : c.proxies) {
    if (p.host.empty()) fail("proxies[" + std::to_string(p.id) + "].host must not be empty");
    if (p.port == 0) fail("proxies[" + std::to_string(p.id) + "].port must be 1-65535");
  }
  if (!is_valid_policy(c.policy)) {
    fail("policy must be one of random, roundrobin, leastconn, latency, weighted; got '" + c.policy + "'");
  }
  if (c.forwarder != "simple" && c.forwarder != "epoll") {
    fail("forwarder must be 'simple' or 'epoll'; got '" + c.forwarder + "'");
  }
  if (c.forwarder_pool.max_connections_per_proxy < 1 || c.forwarder_pool.max_connections_per_proxy > 4096)
    fail("forwarder_pool.max_connections_per_proxy must be between 1 and 4096");
  if (c.forwarder_pool.max_total_connections < 1 || c.forwarder_pool.max_total_connections > 16384)
    fail("forwarder_pool.max_total_connections must be between 1 and 16384");
  if (c.forwarder_pool.max_connections_per_proxy > c.forwarder_pool.max_total_connections)
    fail("forwarder_pool.max_connections_per_proxy must not exceed max_total_connections");
  if (c.forwarder_pool.idle_timeout_ms < 1)
    fail("forwarder_pool.idle_timeout_ms must be >= 1");
  if (!(c.default_rate.rate > 0) || !(c.default_rate.burst > 0)) {
    fail("rate_limits.default rate and burst must be > 0");
  }
  for (const auto& [domain, r] : c.domain_rates) {
    if (!(r.rate > 0) || !(r.burst > 0)) fail("rate_limits." + domain + " rate and burst must be > 0");
  }
  if (c.queue_limit == 0) fail("queue_limit must be >= 1");
  if (c.workers.min < 1) fail("workers.min must be >= 1");
  if (c.workers.max < c.workers.min) {
    fail("workers.max (" + std::to_string(c.workers.max) + ") must be >= workers.min (" +
         std::to_string(c.workers.min) + ")");
  }
  if (!(c.workers.alpha > 0)) fail("workers.alpha must be > 0");
  if (!(c.workers.beta > 0 && c.workers.beta < 1)) fail("workers.beta must be strictly between 0 and 1");
  if (c.connect_timeout_ms <= 0) fail("timeouts.connect_ms must be > 0");
  if (c.response_timeout_ms <= 0) fail("timeouts.response_ms must be > 0");
  if (c.circuit_threshold < 1) fail("circuit.threshold must be >= 1");
  if (c.circuit_cooldown_ms <= 0) fail("circuit.cooldown_ms must be > 0");
  if (c.health_probe_interval_ms <= 0) fail("circuit.probe_interval_ms must be > 0");
  if (c.health_window_size < 1 || c.health_window_size > 1000000) fail("circuit.window_size must be between 1 and 1000000");
  if (c.weights.latency < 0 || c.weights.load < 0 || c.weights.failure < 0) {
    fail("weights must be >= 0");
  }
  if (c.ingress.threads < 1) fail("ingress.threads must be >= 1");
  if (c.ingress.max_header_bytes < 256) fail("ingress.max_header_bytes must be >= 256");
  if (c.ingress.max_body_bytes < 1) fail("ingress.max_body_bytes must be >= 1");
  if (c.ingress.read_timeout_ms <= 0) fail("ingress.read_timeout_ms must be > 0");
  if (c.ingress.pending_connections < 1) fail("ingress.pending_connections must be >= 1");
  if (c.output.format != "none" && c.output.format != "csv" && c.output.format != "json") {
    fail("output.format must be none, csv or json; got '" + c.output.format + "'");
  }
  if (c.output.format != "none" && c.output.path.empty()) {
    fail("output.path is required when output.format is csv or json");
  }
}

GatewayConfig parse_config_text(const std::string& text) {
  json j;
  try {
    j = json::parse(text);
  } catch (const json::parse_error& e) {
    throw ConfigError(std::string("config: invalid JSON: ") + e.what());
  }
  check_keys(j, "", {"listen", "admin", "proxies", "policy", "forwarder", "rate_limits", "queue_limit",
                     "forwarder_pool", "workers", "timeouts", "circuit", "weights", "ingress", "output"});

  GatewayConfig c;

  if (const json* o = read_object(j, "listen")) {
    check_keys(*o, "listen", {"host", "port"});
    c.listen_host = read_string(*o, "host", "listen", c.listen_host);
    c.listen_port = static_cast<uint16_t>(read_int(*o, "port", "listen", c.listen_port, 1, 65535));
  }
  if (const json* o = read_object(j, "admin")) {
    check_keys(*o, "admin", {"host", "port"});
    c.admin_host = read_string(*o, "host", "admin", c.admin_host);
    c.admin_port = static_cast<uint16_t>(read_int(*o, "port", "admin", c.admin_port, 1, 65535));
  }

  if (const json* o = read_object(j, "proxies")) {
    if (!o->is_array()) fail("proxies must be an array");
    ProxyId next = 0;
    for (const auto& p : *o) {
      std::string where = "proxies[" + std::to_string(next) + "]";
      check_keys(p, where, {"host", "port"});
      if (!p.contains("host")) fail(where + ".host is required");
      if (!p.contains("port")) fail(where + ".port is required");
      ProxyEndpoint e;
      e.id = next++;
      e.host = read_string(p, "host", where, "");
      e.port = static_cast<uint16_t>(read_int(p, "port", where, 0, 1, 65535));
      c.proxies.push_back(std::move(e));
    }
  }

  c.policy = read_string(j, "policy", "", c.policy);
  c.forwarder = read_string(j, "forwarder", "", c.forwarder);
  if (const json* o = read_object(j, "forwarder_pool")) {
    check_keys(*o, "forwarder_pool", {"max_connections_per_proxy", "max_total_connections", "idle_timeout_ms"});
    c.forwarder_pool.max_connections_per_proxy = static_cast<size_t>(read_int(
        *o, "max_connections_per_proxy", "forwarder_pool", c.forwarder_pool.max_connections_per_proxy, 1, 4096));
    c.forwarder_pool.max_total_connections = static_cast<size_t>(read_int(
        *o, "max_total_connections", "forwarder_pool", c.forwarder_pool.max_total_connections, 1, 16384));
    c.forwarder_pool.idle_timeout_ms = static_cast<int>(read_int(
        *o, "idle_timeout_ms", "forwarder_pool", c.forwarder_pool.idle_timeout_ms, 1, 3600000));
  }

  if (const json* o = read_object(j, "rate_limits")) {
    if (!o->is_object()) fail("rate_limits must be an object");
    for (auto it = o->begin(); it != o->end(); ++it) {
      std::string where = "rate_limits." + it.key();
      if (it.key() == "default") {
        c.default_rate = read_rate(it.value(), where, c.default_rate);
      } else {
        c.domain_rates[to_lower(it.key())] = read_rate(it.value(), where, c.default_rate);
      }
    }
    // Domains that did not set a field inherit the (possibly overridden) default.
    for (auto it = o->begin(); it != o->end(); ++it) {
      if (it.key() == "default") continue;
      auto& r = c.domain_rates[to_lower(it.key())];
      if (!it.value().contains("rate")) r.rate = c.default_rate.rate;
      if (!it.value().contains("burst")) r.burst = c.default_rate.burst;
    }
  }
  c.queue_limit = static_cast<size_t>(read_int(j, "queue_limit", "", static_cast<long long>(c.queue_limit), 1, 10000000));

  if (const json* o = read_object(j, "workers")) {
    check_keys(*o, "workers", {"min", "max", "alpha", "beta"});
    c.workers.min = static_cast<int>(read_int(*o, "min", "workers", c.workers.min, 1, 100000));
    c.workers.max = static_cast<int>(read_int(*o, "max", "workers", c.workers.max, 1, 100000));
    c.workers.alpha = read_double(*o, "alpha", "workers", c.workers.alpha);
    c.workers.beta = read_double(*o, "beta", "workers", c.workers.beta);
  }
  if (const json* o = read_object(j, "timeouts")) {
    check_keys(*o, "timeouts", {"connect_ms", "response_ms"});
    c.connect_timeout_ms = static_cast<int>(read_int(*o, "connect_ms", "timeouts", c.connect_timeout_ms, 1, 3600000));
    c.response_timeout_ms = static_cast<int>(read_int(*o, "response_ms", "timeouts", c.response_timeout_ms, 1, 3600000));
  }
  if (const json* o = read_object(j, "circuit")) {
    check_keys(*o, "circuit", {"threshold", "cooldown_ms", "probe_interval_ms", "window_size"});
    c.circuit_threshold = static_cast<int>(read_int(*o, "threshold", "circuit", c.circuit_threshold, 1, 1000000));
    c.circuit_cooldown_ms = static_cast<int>(read_int(*o, "cooldown_ms", "circuit", c.circuit_cooldown_ms, 1, 3600000));
    c.health_probe_interval_ms = static_cast<int>(read_int(*o, "probe_interval_ms", "circuit", c.health_probe_interval_ms, 1, 3600000));
    c.health_window_size = static_cast<size_t>(read_int(*o, "window_size", "circuit", c.health_window_size, 1, 1000000));
  }
  if (const json* o = read_object(j, "weights")) {
    check_keys(*o, "weights", {"latency", "load", "failure"});
    c.weights.latency = read_double(*o, "latency", "weights", c.weights.latency);
    c.weights.load = read_double(*o, "load", "weights", c.weights.load);
    c.weights.failure = read_double(*o, "failure", "weights", c.weights.failure);
  }
  if (const json* o = read_object(j, "ingress")) {
    check_keys(*o, "ingress", {"threads", "max_header_bytes", "max_body_bytes", "read_timeout_ms", "pending_connections"});
    c.ingress.threads = static_cast<int>(read_int(*o, "threads", "ingress", c.ingress.threads, 1, 4096));
    c.ingress.max_header_bytes = static_cast<size_t>(
        read_int(*o, "max_header_bytes", "ingress", static_cast<long long>(c.ingress.max_header_bytes), 256, 1 << 20));
    c.ingress.max_body_bytes = static_cast<size_t>(
        read_int(*o, "max_body_bytes", "ingress", static_cast<long long>(c.ingress.max_body_bytes), 1, 1LL << 30));
    c.ingress.read_timeout_ms = static_cast<int>(read_int(*o, "read_timeout_ms", "ingress", c.ingress.read_timeout_ms, 1, 3600000));
    c.ingress.pending_connections = static_cast<int>(
        read_int(*o, "pending_connections", "ingress", c.ingress.pending_connections, 1, 1000000));
  }
  if (const json* o = read_object(j, "output")) {
    check_keys(*o, "output", {"format", "path"});
    c.output.format = read_string(*o, "format", "output", c.output.format);
    c.output.path = read_string(*o, "path", "output", c.output.path);
  }

  validate_config(c);
  return c;
}

GatewayConfig load_config_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw ConfigError("config: cannot open file '" + path + "'");
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_config_text(ss.str());
}

std::string usage_text() {
  return "Usage: gateway --config FILE [--policy NAME] [--workers N] [--help]\n"
         "\n"
         "Options:\n"
         "  --config FILE    Path to the gateway JSON configuration file (required).\n"
         "  --policy NAME    Routing policy: random, roundrobin, leastconn, latency, weighted.\n"
         "                   Overrides the config file. (Routing is enforced from Phase 3.)\n"
         "  --workers N      Maximum worker count; overrides workers.max in the config file.\n"
         "  --help, -h       Show this help and exit.\n"
         "\n"
         "Exit status: 0 on success, 2 on invalid options or configuration, 1 on runtime failure.\n";
}

CliOptions parse_cli(int argc, const char* const* argv) {
  CliOptions o;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    std::string name = arg;
    std::optional<std::string> inline_value;
    if (arg.rfind("--", 0) == 0) {
      size_t eq = arg.find('=');
      if (eq != std::string::npos) {
        name = arg.substr(0, eq);
        inline_value = arg.substr(eq + 1);
      }
    }
    auto value = [&]() -> std::string {
      if (inline_value) return *inline_value;
      if (i + 1 >= argc) throw ConfigError("option " + name + " requires a value");
      return argv[++i];
    };
    if (name == "--help" || name == "-h") {
      o.help = true;
    } else if (name == "--config") {
      o.config_path = value();
      if (o.config_path.empty()) throw ConfigError("option --config requires a non-empty path");
    } else if (name == "--policy") {
      std::string p = value();
      if (!is_valid_policy(p)) {
        throw ConfigError("invalid --policy '" + p + "' (expected random, roundrobin, leastconn, latency or weighted)");
      }
      o.policy = p;
    } else if (name == "--workers") {
      std::string v = value();
      unsigned long long n = 0;
      if (!parse_uint(v, 100000, n) || n < 1) {
        throw ConfigError("invalid --workers '" + v + "' (expected an integer between 1 and 100000)");
      }
      o.workers = static_cast<int>(n);
    } else {
      throw ConfigError("unknown option '" + arg + "' (try --help)");
    }
  }
  return o;
}

void apply_cli_overrides(GatewayConfig& cfg, const CliOptions& opts) {
  if (opts.policy) cfg.policy = *opts.policy;
  if (opts.workers) cfg.workers.max = *opts.workers;
  validate_config(cfg);
}

}  // namespace pg
