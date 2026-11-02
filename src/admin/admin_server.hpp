#pragma once
// Admin API and static dashboard server. Serves live metrics/proxy snapshots using the stable
// JSON shapes from docs/INTERFACES.md. Bound to localhost by default (SRS security requirement).
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "common/config.hpp"
#include "common/fd.hpp"
#include "common/metrics.hpp"
#include "common/types.hpp"
#include "output/result_writer.hpp"

namespace pg {

struct Phase2Stats { std::map<std::string, size_t> queue_length; int worker_count = 0; int aimd_limit = 0; int active_jobs = 0; };

class AdminServer {
 public:
  AdminServer(const GatewayConfig& cfg, Metrics& metrics, std::function<Phase2Stats()> phase2 = {},
              std::function<std::vector<ProxyState>()> proxy_snapshot = {},
              std::function<std::vector<RequestResult>()> recent_snapshot = {},
              std::function<std::string()> policy_snapshot = {},
              std::function<bool(const std::string&)> policy_setter = {},
              std::function<void(double,double)> rate_setter = {},
              std::function<bool()> health_aware_snapshot = {},
              std::function<void(bool)> health_aware_setter = {});
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
  std::function<std::vector<RequestResult>()> recent_snapshot_;
  std::function<std::string()> policy_snapshot_;
  std::function<bool(const std::string&)> policy_setter_;
  std::function<void(double,double)> rate_setter_;
  std::function<bool()> health_aware_snapshot_;
  std::function<void(bool)> health_aware_setter_;
  Fd listen_fd_;
  uint16_t bound_port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  mutable std::mutex rate_mu_;
  mutable uint64_t rate_last_total_ = 0;
  mutable std::chrono::steady_clock::time_point rate_last_sample_ = std::chrono::steady_clock::now();
  mutable double requests_per_second_ = 0.0;
  double update_requests_per_second() const;
};

}  // namespace pg
