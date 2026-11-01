#include "health/health_monitor.hpp"

#include <algorithm>
#include <chrono>

namespace pg {

HealthMonitor::HealthMonitor(const GatewayConfig& cfg, GatewayCore& core)
    : cfg_(cfg), core_(core), probe_forwarder_(std::min(cfg.connect_timeout_ms, 1000),
                                               std::min(cfg.response_timeout_ms, 1000)) {}
HealthMonitor::~HealthMonitor() { stop(); }

void HealthMonitor::start() {
  if (!threads_.empty()) return;
  stopping_ = false;
  for (const auto& proxy : core_.proxies())
    threads_.emplace_back([this, id = proxy.id] { loop(id); });
}

void HealthMonitor::stop() {
  stopping_ = true;
  for (auto& thread : threads_) if (thread.joinable()) thread.join();
  threads_.clear();
}

void HealthMonitor::loop(ProxyId proxy_id) {
  using namespace std::chrono;
  while (!stopping_) {
    const auto next = steady_clock::now() + milliseconds(cfg_.health_probe_interval_ms);
    // Each configured proxy has one bounded monitor thread; probes never hold the state lock
    // during forwarding and cannot delay another proxy's detection cadence.
    core_.probe_proxy(proxy_id, &probe_forwarder_);
    while (!stopping_ && steady_clock::now() < next)
      std::this_thread::sleep_for(milliseconds(25));
  }
}

}  // namespace pg
