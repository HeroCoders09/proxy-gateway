#pragma once
// STUB admin server (Phase 1). Serves the documented JSON shapes from docs/INTERFACES.md with
// dummy data (plus the real Phase 1 counters), so Phase 6 can build the dashboard against stable
// field names. Phase 6 replaces the data sources; it must keep the field names.
// Bound to localhost by default (SRS security requirement).
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <thread>

#include "common/config.hpp"
#include "common/fd.hpp"
#include "common/metrics.hpp"
#include "common/types.hpp"

namespace pg {

struct Phase2Stats { std::map<std::string, size_t> queue_length; int worker_count = 0; int aimd_limit = 0; int active_jobs = 0; };

class AdminServer {
 public:
  AdminServer(const GatewayConfig& cfg, Metrics& metrics, std::function<Phase2Stats()> phase2 = {},
              std::function<std::vector<ProxyState>()> proxy_snapshot = {});
  ~AdminServer();
  AdminServer(const AdminServer&) = delete;
  AdminServer& operator=(const AdminServer&) = delete;

  void start();  // throws std::runtime_error if it cannot bind
  void stop();
  uint16_t port() const { return bound_port_; }

  // Exposed for tests: route a request line target to (status, content_type, body).
  struct Reply {
    int status = 200;
    std::string content_type;
    std::string body;
  };
  Reply route(const std::string& method, const std::string& target) const;

 private:
  void loop();
  void serve(Fd client);

  const GatewayConfig cfg_;
  Metrics& metrics_;
  std::function<Phase2Stats()> phase2_;
  std::function<std::vector<ProxyState>()> proxy_snapshot_;
  Fd listen_fd_;
  uint16_t bound_port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

}  // namespace pg
