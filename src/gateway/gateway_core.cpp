#include "gateway/gateway_core.hpp"

#include <algorithm>
#include <functional>

#include "common/http_util.hpp"
#include "common/log.hpp"
#include "routing/policy_router.hpp"

namespace pg {
namespace {

class Completion {
 public:
  explicit Completion(std::function<void()> fn) : fn_(std::move(fn)) {}
  ~Completion() { if (fn_) fn_(); }
  Completion(const Completion&) = delete;
  Completion& operator=(const Completion&) = delete;
 private:
  std::function<void()> fn_;
};

Counter selection_counter(const std::string& policy) {
  if (policy == "random") return Counter::RandomSelections;
  if (policy == "leastconn") return Counter::LeastConnSelections;
  if (policy == "latency") return Counter::LatencySelections;
  if (policy == "weighted") return Counter::WeightedSelections;
  return Counter::RoundRobinSelections;
}

}  // namespace

GatewayCore::GatewayCore(const GatewayConfig& cfg, Forwarder& forwarder, MetricsSink& metrics)
    : forwarder_(forwarder), metrics_(metrics), policy_(cfg.policy),
      router_(std::make_unique<PolicyRouter>(cfg.policy, cfg.weights)) {
  for (const auto& p : cfg.proxies) {
    ProxyState state;
    state.id = p.id;
    state.host = p.host;
    state.port = p.port;
    proxies_.push_back(std::move(state));
  }
}

std::vector<ProxyState> GatewayCore::proxies() const {
  std::lock_guard<std::mutex> lock(proxies_mu_);
  return proxies_;
}

void GatewayCore::report(ProxyId proxy_id, bool success, double rtt_ms) {
  std::lock_guard<std::mutex> lock(proxies_mu_);
  auto it = std::find_if(proxies_.begin(), proxies_.end(), [proxy_id](const auto& p) { return p.id == proxy_id; });
  if (it == proxies_.end()) return;
  if (success) {
    ++it->success_count;
    it->consecutive_failures = 0;
  } else {
    ++it->failure_count;
    ++it->consecutive_failures;
  }
  if (rtt_ms > 0.0) it->rtt_ms = it->rtt_ms > 0.0 ? (0.8 * it->rtt_ms + 0.2 * rtt_ms) : rtt_ms;
}

HttpResponse GatewayCore::handle(Job& job) {
  constexpr int kMaxRetries = 2;
  int attempts = 0;
  bool has_last_error = false;
  ForwardResult last_error;
  ProxyId last_error_proxy = -1;
  for (;;) {
    ProxyState selected;
    bool routed = false;
    {
      // Serialize selection plus reservation so least-connection decisions see every in-flight
      // selection, even when several worker threads wake at once.
      std::lock_guard<std::mutex> selection_lock(selection_mu_);
      std::vector<ProxyState> snapshot;
      {
        std::lock_guard<std::mutex> state_lock(proxies_mu_);
        snapshot = proxies_;
      }
      const auto id = router_->select(job, snapshot);
      if (id) {
        std::lock_guard<std::mutex> state_lock(proxies_mu_);
        auto it = std::find_if(proxies_.begin(), proxies_.end(), [id](const auto& p) { return p.id == *id; });
        if (it != proxies_.end()) {
          ++it->active_connections;
          selected = *it;
          routed = true;
        }
      }
    }

    if (!routed) {
      metrics_.inc(Counter::NoRoute);
      metrics_.inc(Counter::RequestsFailed);
      if (has_last_error) {
        HttpResponse response = make_error_response(is_timeout(last_error.error) ? 504 : 502,
                                                    to_string(last_error.error));
        response.headers.push_back({"X-Gateway-Proxy", std::to_string(last_error_proxy)});
        return response;
      }
      return make_error_response(502, "no eligible proxy");
    }
    if (attempts > 0) {
      ++job.retry_count;
      metrics_.inc(Counter::Retries);
    }
    metrics_.inc(selection_counter(policy_));
    job.tried_proxies.push_back(selected.id);
    Completion release([this, id = selected.id] {
      std::lock_guard<std::mutex> lock(proxies_mu_);
      auto it = std::find_if(proxies_.begin(), proxies_.end(), [id](const auto& p) { return p.id == id; });
      if (it != proxies_.end() && it->active_connections > 0) --it->active_connections;
    });

    ForwardResult result;
    try {
      result = forwarder_.forward(job, selected);
    } catch (...) {
      report(selected.id, false, 0.0);
      metrics_.inc(Counter::RequestsFailed);
      return make_error_response(502, "proxy attempt failed");
    }
    report(selected.id, result.ok() && result.status < 500, result.latency_ms);

    if (!result.ok()) {
      has_last_error = true;
      last_error = result;
      last_error_proxy = selected.id;
      if (is_timeout(result.error)) metrics_.inc(Counter::Timeouts);
      log_warn("job " + std::to_string(job.id) + " via proxy " + std::to_string(selected.id) + " failed: " +
               to_string(result.error) + " (" + result.error_detail + ")");
      if (attempts < kMaxRetries) {
        ++attempts;
        continue;
      }
      metrics_.inc(Counter::RequestsFailed);
      HttpResponse response = make_error_response(is_timeout(result.error) ? 504 : 502, to_string(result.error));
      response.headers.push_back({"X-Gateway-Proxy", std::to_string(selected.id)});
      return response;
    }

    metrics_.observe_latency_ms(result.latency_ms);
    metrics_.inc(result.status < 500 ? Counter::RequestsSuccessful : Counter::RequestsFailed);
    HttpResponse response;
    response.status = result.status;
    response.headers = std::move(result.headers);
    strip_hop_by_hop(response.headers);
    response.headers.push_back({"X-Gateway-Proxy", std::to_string(selected.id)});
    response.body = std::move(result.body);
    return response;
  }
}

}  // namespace pg
