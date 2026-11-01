#include "gateway/gateway_core.hpp"

#include "common/http_util.hpp"
#include "common/log.hpp"

namespace pg {

GatewayCore::GatewayCore(const GatewayConfig& cfg, Forwarder& forwarder, MetricsSink& metrics)
    : forwarder_(forwarder), metrics_(metrics) {
  for (const auto& p : cfg.proxies) {
    ProxyState s;
    s.id = p.id;
    s.host = p.host;
    s.port = p.port;
    proxies_.push_back(std::move(s));
  }
}

HttpResponse GatewayCore::handle(Job& job) {
  // Phase 1: always the first configured proxy. Routing policies arrive in Phase 3.
  const ProxyState& proxy = proxies_.front();
  job.tried_proxies.push_back(proxy.id);

  ForwardResult r = forwarder_.forward(job, proxy);

  if (!r.ok()) {
    metrics_.inc(Counter::RequestsFailed);
    int status = 502;
    if (is_timeout(r.error)) {
      metrics_.inc(Counter::Timeouts);
      status = 504;
    }
    // Log only metadata: never URLs with credentials or bodies.
    log_warn("job " + std::to_string(job.id) + " via proxy " + std::to_string(proxy.id) + " failed: " +
             to_string(r.error) + " (" + r.error_detail + ")");
    HttpResponse resp = make_error_response(status, to_string(r.error));
    resp.headers.push_back({"X-Gateway-Proxy", std::to_string(proxy.id)});
    return resp;
  }

  metrics_.observe_latency_ms(r.latency_ms);
  // D-010: a response counts as successful when transport worked and status < 500.
  metrics_.inc(r.status < 500 ? Counter::RequestsSuccessful : Counter::RequestsFailed);

  HttpResponse resp;
  resp.status = r.status;
  resp.headers = std::move(r.headers);
  strip_hop_by_hop(resp.headers);
  resp.headers.push_back({"X-Gateway-Proxy", std::to_string(proxy.id)});
  resp.body = std::move(r.body);
  return resp;
}

}  // namespace pg
