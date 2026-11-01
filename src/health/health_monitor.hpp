#pragma once

#include <atomic>
#include <vector>
#include <thread>

#include "common/config.hpp"
#include "gateway/gateway_core.hpp"
#include "net/simple_forwarder.hpp"

namespace pg {

class HealthMonitor {
 public:
  HealthMonitor(const GatewayConfig& cfg, GatewayCore& core);
  ~HealthMonitor();
  void start();
  void stop();

 private:
  void loop(ProxyId proxy_id);
  const GatewayConfig& cfg_;
  GatewayCore& core_;
  std::atomic<bool> stopping_{false};
  SimpleForwarder probe_forwarder_;
  std::vector<std::thread> threads_;
};

}  // namespace pg
