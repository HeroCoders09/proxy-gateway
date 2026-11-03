#pragma once
// Shared data structures (handbook section 3.3). Fields may be added but not renamed or
// removed without a DECISIONS.md entry.
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "common/clock.hpp"
#include "common/fd.hpp"

namespace pg {

using ProxyId = int;

struct Header {
  std::string name;
  std::string value;
};
using Headers = std::vector<Header>;

enum class CircuitState { Closed, Open, HalfOpen };

inline const char* to_string(CircuitState s) {
  switch (s) {
    case CircuitState::Closed: return "CLOSED";
    case CircuitState::Open: return "OPEN";
    case CircuitState::HalfOpen: return "HALF_OPEN";
  }
  return "CLOSED";
}

struct Job {
  uint64_t id = 0;
  std::string url;      // absolute URL as received
  std::string domain;   // lower-case host, no port: the rate-limit / queue key
  std::string method;
  Headers headers;
  std::string body;
  TimePoint enqueue_time{};
  int retry_count = 0;
  std::vector<ProxyId> tried_proxies;
  int client_fd = -1;   // client connection handle; NOT owned (the ingress owns it)
  // Parsed pieces of `url` (added in Phase 1 so later phases do not re-parse).
  std::string target_host;  // as written, lower-cased
  uint16_t target_port = 80;
  std::string target_path;  // path + query, always starts with '/'
};

struct ProxyState {
  ProxyId id = -1;
  std::string host;
  uint16_t port = 0;
  // Upstream proxy credentials are runtime-only and intentionally excluded from admin JSON.
  std::string auth_username;
  std::string auth_password;
  bool healthy = true;
  double rtt_ms = 0.0;  // recent RTT
  uint64_t success_count = 0;
  uint64_t failure_count = 0;
  uint32_t consecutive_failures = 0;
  int active_connections = 0;
  TimePoint last_probe_time{};
  CircuitState circuit_state = CircuitState::Closed;
  double rolling_success_rate = 1.0;
  double rolling_rtt_ms = 0.0;
  uint64_t circuit_open_count = 0;
  uint64_t probe_success_count = 0;
  uint64_t probe_failure_count = 0;
  int64_t last_probe_unix_ms = 0;
  // Internal breaker fields are copied with snapshots but are not part of admin JSON.
  TimePoint circuit_opened_at{};
  bool half_open_probe_in_flight = false;
};

struct DomainQueue {
  std::deque<Job> jobs;
  size_t queue_limit = 10000;
  double tokens = 0.0;
  double refill_rate = 0.0;  // tokens per second
  double capacity = 0.0;     // burst B
  TimePoint last_refill{};
};

enum class ConnState { Idle, Connecting, Active, Closed };

struct Connection {
  Fd fd;
  ProxyId proxy_id = -1;
  ConnState state = ConnState::Closed;
  TimePoint last_used{};       // keep-alive metadata
  uint32_t requests_served = 0;
  bool keep_alive = true;
  std::string in_buf;          // pending buffers
  std::string out_buf;
};

}  // namespace pg
