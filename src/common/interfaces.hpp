#pragma once
// Fixed interfaces between modules (handbook section 3.4). Documented in docs/INTERFACES.md.
#include <optional>
#include <string>
#include <vector>

#include "common/metrics.hpp"
#include "common/types.hpp"

namespace pg {

enum class ErrorKind {
  None,
  ConnectTimeout,   // connect did not finish within connect_timeout
  ResponseTimeout,  // no complete response within response_timeout
  ConnectFailed,    // refused / unreachable / DNS failure
  IoError,          // reset, broken pipe, closed without a response
  BadResponse       // response could not be parsed or was too large
};

inline const char* to_string(ErrorKind k) {
  switch (k) {
    case ErrorKind::None: return "none";
    case ErrorKind::ConnectTimeout: return "connect_timeout";
    case ErrorKind::ResponseTimeout: return "response_timeout";
    case ErrorKind::ConnectFailed: return "connect_failed";
    case ErrorKind::IoError: return "io_error";
    case ErrorKind::BadResponse: return "bad_response";
  }
  return "none";
}

inline bool is_timeout(ErrorKind k) {
  return k == ErrorKind::ConnectTimeout || k == ErrorKind::ResponseTimeout;
}

struct ForwardResult {
  ErrorKind error = ErrorKind::None;
  std::string error_detail;  // short, never contains response bodies or credentials
  int status = 0;
  Headers headers;
  std::string body;
  double latency_ms = 0.0;   // connect + request + response, wall clock
  ProxyId proxy_id = -1;
  bool ok() const { return error == ErrorKind::None; }
};

// forward(job, proxy) -> Result. Simple (Phase 1) and epoll (Phase 5) forwarders implement it.
class Forwarder {
 public:
  virtual ~Forwarder() = default;
  virtual ForwardResult forward(const Job& job, const ProxyState& proxy) = 0;
};

// select(job, proxy_table) -> optional<ProxyId>; nullopt means "no route available".
class Router {
 public:
  virtual ~Router() = default;
  virtual std::optional<ProxyId> select(const Job& job, const std::vector<ProxyState>& table) = 0;
};

// try_acquire(domain) -> bool, driven by a Clock that tests can fake.
class RateLimiter {
 public:
  virtual ~RateLimiter() = default;
  virtual bool try_acquire(const std::string& domain) = 0;
};

// Called after every attempt and every probe.
class HealthReporter {
 public:
  virtual ~HealthReporter() = default;
  virtual void report(ProxyId proxy_id, bool success, double rtt_ms) = 0;
};

}  // namespace pg
