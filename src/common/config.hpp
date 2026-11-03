#pragma once
// GatewayConfig + loader/validator + CLI parsing. Config is external JSON or CLI flags;
// changing a policy never requires recompiling. Keys are documented in docs/INTERFACES.md.
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace pg {

class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct ProxyEndpoint {
  ProxyId id = -1;  // index in the proxies array
  std::string host;
  uint16_t port = 0;
  std::string auth_username;
  std::string auth_password_env;
  std::string auth_password;  // loaded at startup; never returned by an API
};

struct RateLimitCfg {
  double rate = 1000.0;   // tokens per second (r)
  double burst = 2000.0;  // bucket capacity (B)
};

struct WorkersCfg {
  int min = 4;
  int max = 64;
  double alpha = 1.0;  // additive increase
  double beta = 0.5;   // multiplicative decrease, 0 < beta < 1
};

struct WeightsCfg {  // weighted-score policy (Phase 3 may retune the defaults)
  double latency = 0.5;
  double load = 0.3;
  double failure = 0.2;
};

struct IngressCfg {
  int threads = 16;                 // fixed connection-handler pool (no thread per request)
  size_t max_header_bytes = 16384;
  size_t max_body_bytes = 1048576;
  int read_timeout_ms = 5000;       // client must deliver a full request within this time
  int pending_connections = 1024;   // bounded accept queue
};

struct OutputCfg {
  std::string format = "none";  // none | csv | json (JSON Lines)
  std::string path;
};

struct ClientAuthCfg {
  bool required = false;
  std::string username = "pricelens";
  std::string password_env = "PROXY_GATEWAY_CLIENT_PASSWORD";
  std::string password;  // loaded at startup from password_env; never returned by an API
};

struct EgressCfg {
  bool allowlist_enabled = false;
  std::vector<std::string> allowed_hosts;  // exact, lower-case names; no wildcard matching
  std::vector<uint16_t> allowed_ports;
};

struct SecurityCfg {
  ClientAuthCfg client_auth;
  EgressCfg egress;
};

struct ForwarderPoolCfg {
  size_t max_connections_per_proxy = 8;
  size_t max_total_connections = 128;
  int idle_timeout_ms = 30000;
};

struct GatewayConfig {
  std::string listen_host = "0.0.0.0";
  uint16_t listen_port = 8000;
  std::string admin_host = "127.0.0.1";  // localhost by default (SRS security requirement)
  uint16_t admin_port = 9000;

  std::vector<ProxyEndpoint> proxies;
  std::string policy = "roundrobin";  // random|roundrobin|leastconn|latency|weighted
  std::string forwarder = "simple";   // simple|epoll
  ForwarderPoolCfg forwarder_pool;

  RateLimitCfg default_rate;                      // used for domains without an entry
  std::map<std::string, RateLimitCfg> domain_rates;  // lower-case domain -> limit
  size_t queue_limit = 10000;

  WorkersCfg workers;
  int connect_timeout_ms = 2000;
  int response_timeout_ms = 10000;
  int circuit_threshold = 5;
  int circuit_cooldown_ms = 10000;
  int health_probe_interval_ms = 2000;
  size_t health_window_size = 20;
  WeightsCfg weights;
  IngressCfg ingress;
  OutputCfg output;
  SecurityCfg security;
};

// Parse JSON text into a validated config. Throws ConfigError with a clear message.
GatewayConfig parse_config_text(const std::string& json_text);
GatewayConfig load_config_file(const std::string& path);

// Cross-field validation (also run by the loaders). Throws ConfigError.
void validate_config(const GatewayConfig& cfg);

bool is_valid_policy(const std::string& p);

struct CliOptions {
  std::string config_path;
  std::optional<std::string> policy;
  std::optional<int> workers;  // overrides workers.max
  bool help = false;
};

// Throws ConfigError on unknown options, missing values or bad numbers.
CliOptions parse_cli(int argc, const char* const* argv);

// Apply overrides, then re-validate. Throws ConfigError.
void apply_cli_overrides(GatewayConfig& cfg, const CliOptions& opts);

std::string usage_text();

}  // namespace pg
