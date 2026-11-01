#pragma once
// Phase 1 glue: Job -> (single configured proxy) -> Forwarder -> client response + metrics.
// Phase 2 replaces the synchronous call with queue + worker pool; Phase 3 replaces the fixed
// proxy choice with a Router. The success/failure mapping below is decision D-010.
#include <vector>

#include "common/config.hpp"
#include "common/interfaces.hpp"
#include "ingress/http_response.hpp"

namespace pg {

class GatewayCore {
 public:
  GatewayCore(const GatewayConfig& cfg, Forwarder& forwarder, MetricsSink& metrics);

  // Never throws for network problems: they become 502/504 responses.
  HttpResponse handle(Job& job);

  const std::vector<ProxyState>& proxies() const { return proxies_; }

 private:
  Forwarder& forwarder_;
  MetricsSink& metrics_;
  std::vector<ProxyState> proxies_;
};

}  // namespace pg
