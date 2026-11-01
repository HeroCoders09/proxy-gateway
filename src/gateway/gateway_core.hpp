#pragma once
// Phase 1 glue: Job -> (single configured proxy) -> Forwarder -> client response + metrics.
// Phase 2 replaces the synchronous call with queue + worker pool; Phase 3 replaces the fixed
// proxy choice with a Router. The success/failure mapping below is decision D-010.
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "common/config.hpp"
#include "common/interfaces.hpp"
#include "ingress/http_response.hpp"
#include "health/health_state.hpp"

namespace pg {

class GatewayCore : public HealthReporter {
 public:
  GatewayCore(const GatewayConfig& cfg, Forwarder& forwarder, MetricsSink& metrics, const Clock* clock = nullptr);

  // Never throws for network problems: they become 502/504 responses.
  HttpResponse handle(Job& job);

  std::vector<ProxyState> proxies() const;
  void report(ProxyId proxy_id, bool success, double rtt_ms) override;
  bool probe_proxy(ProxyId proxy_id, Forwarder* probe_forwarder = nullptr);

 private:
  Forwarder& forwarder_;
  MetricsSink& metrics_;
  SteadyClock default_clock_;
  const Clock& clock_;
  int circuit_threshold_;
  int circuit_cooldown_ms_;
  size_t health_window_size_;
  std::string policy_;
  std::unique_ptr<Router> router_;
  mutable std::mutex proxies_mu_;
  std::vector<ProxyState> proxies_;
  std::unordered_map<ProxyId, RollingStats> rolling_;
  std::unordered_map<ProxyId, std::unique_ptr<CircuitBreaker>> breakers_;
  std::mutex selection_mu_;
};

}  // namespace pg
