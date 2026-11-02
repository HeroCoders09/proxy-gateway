#include "gateway/gateway_core.hpp"

#include <algorithm>
#include <functional>
#include <chrono>

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

GatewayCore::GatewayCore(const GatewayConfig& cfg, Forwarder& forwarder, MetricsSink& metrics, const Clock* clock)
    : forwarder_(forwarder), metrics_(metrics), clock_(clock ? *clock : default_clock_),
      circuit_threshold_(cfg.circuit_threshold), circuit_cooldown_ms_(cfg.circuit_cooldown_ms),
      health_window_size_(cfg.health_window_size), policy_(cfg.policy),
      weights_(cfg.weights),
      router_(std::make_unique<PolicyRouter>(cfg.policy, cfg.weights)) {
  for (const auto& p : cfg.proxies) {
    ProxyState state;
    state.id = p.id;
    state.host = p.host;
    state.port = p.port;
    proxies_.push_back(std::move(state));
    rolling_.emplace(p.id, RollingStats(health_window_size_));
    breakers_.emplace(p.id, std::make_unique<CircuitBreaker>(circuit_threshold_,
        std::chrono::milliseconds(circuit_cooldown_ms_), clock_));
  }
}

std::vector<ProxyState> GatewayCore::proxies() const {
  std::lock_guard<std::mutex> lock(proxies_mu_);
  return proxies_;
}

bool GatewayCore::set_policy(const std::string& policy) {
  if (!is_valid_policy(policy)) return false;
  std::lock_guard<std::mutex> lock(selection_mu_);
  policy_ = policy;
  router_ = std::make_unique<PolicyRouter>(policy, weights_);
  return true;
}

std::string GatewayCore::policy() const {
  std::lock_guard<std::mutex> lock(selection_mu_);
  return policy_;
}

void GatewayCore::set_health_aware(bool enabled) {
  std::lock_guard<std::mutex> lock(selection_mu_);
  health_aware_ = enabled;
}

bool GatewayCore::health_aware() const {
  std::lock_guard<std::mutex> lock(selection_mu_);
  return health_aware_;
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
  auto& window = rolling_.at(proxy_id);
  window.add(success, rtt_ms);
  it->rolling_success_rate = window.success_rate();
  it->rolling_rtt_ms = window.mean_rtt_ms();
  const size_t health_samples = std::min<size_t>(4, health_window_size_);
  if (window.size() >= health_samples && window.recent_success_rate(health_samples) < 0.5) it->healthy = false;
  else it->healthy = true;
  auto& breaker = *breakers_.at(proxy_id);
  const auto prior_open_count = breaker.open_count();
  breaker.record(success);
  it->consecutive_failures = breaker.consecutive_failures();
  it->circuit_state = breaker.state();
  it->circuit_open_count = breaker.open_count();
  if (it->circuit_open_count > prior_open_count) it->circuit_opened_at = clock_.now();
}

bool GatewayCore::probe_proxy(ProxyId proxy_id, Forwarder* probe_forwarder) {
  ProxyState selected;
  {
    std::lock_guard<std::mutex> lock(proxies_mu_);
    auto it = std::find_if(proxies_.begin(), proxies_.end(), [proxy_id](const auto& p) { return p.id == proxy_id; });
    if (it == proxies_.end()) return false;
    auto& breaker = *breakers_.at(proxy_id);
    if (!breaker.begin_probe()) return false;
    it->circuit_state = breaker.state();
    it->half_open_probe_in_flight = breaker.probe_in_flight();
    selected = *it;
  }

  Job job;
  job.method = "GET";
  job.url = "http://test.example/health";
  job.domain = "test.example";
  job.target_host = "test.example";
  job.target_port = 80;
  job.target_path = "/health";
  ForwardResult result;
  try {
    result = (probe_forwarder ? *probe_forwarder : forwarder_).forward(job, selected);
  } catch (...) {
    result.error = ErrorKind::IoError;
  }
  const bool success = result.ok() && result.status >= 200 && result.status < 300;
  {
    std::lock_guard<std::mutex> lock(proxies_mu_);
    auto it = std::find_if(proxies_.begin(), proxies_.end(), [proxy_id](const auto& p) { return p.id == proxy_id; });
    if (it == proxies_.end()) return false;
    auto& breaker = *breakers_.at(proxy_id);
    const auto prior_open_count = breaker.open_count();
    breaker.finish_probe(success);
    it->half_open_probe_in_flight = breaker.probe_in_flight();
    it->circuit_state = breaker.state();
    it->consecutive_failures = breaker.consecutive_failures();
    it->circuit_open_count = breaker.open_count();
    if (it->circuit_open_count > prior_open_count) it->circuit_opened_at = clock_.now();
    it->last_probe_time = clock_.now();
    it->last_probe_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ++(success ? it->probe_success_count : it->probe_failure_count);
    if (result.latency_ms > 0.0) it->rtt_ms = it->rtt_ms > 0.0 ? (0.8 * it->rtt_ms + 0.2 * result.latency_ms) : result.latency_ms;
    auto& window = rolling_.at(proxy_id);
    window.add(success, result.latency_ms);
    it->rolling_success_rate = window.success_rate();
    it->rolling_rtt_ms = window.mean_rtt_ms();
    const size_t health_samples = std::min<size_t>(4, health_window_size_);
    it->healthy = !(window.size() >= health_samples && window.recent_success_rate(health_samples) < 0.5);
    metrics_.inc(success ? Counter::ProbeSuccesses : Counter::ProbeFailures);
  }
  return success;
}

HttpResponse GatewayCore::handle(Job& job) {
  const auto started = std::chrono::steady_clock::now();
  double upstream_ms = 0.0;
  HttpResponse response = handle_impl(job, upstream_ms);
  const double elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - started).count();
  metrics_.observe_gateway_overhead_ms(std::max(0.0, elapsed - upstream_ms));
  return response;
}

HttpResponse GatewayCore::handle_impl(Job& job, double& upstream_ms) {
  constexpr int kMaxRetries = 2;
  int attempts = 0;
  bool has_last_error = false;
  ForwardResult last_error;
  ProxyId last_error_proxy = -1;
  for (;;) {
    ProxyState selected;
    bool routed = false;
    std::string selected_policy;
    {
      // Serialize selection plus reservation so least-connection decisions see every in-flight
      // selection, even when several worker threads wake at once.
      std::lock_guard<std::mutex> selection_lock(selection_mu_);
      std::vector<ProxyState> snapshot;
      {
        std::lock_guard<std::mutex> state_lock(proxies_mu_);
        snapshot = proxies_;
      }
      if (!health_aware_) for (auto& proxy : snapshot) proxy.healthy = true;
      const auto id = router_->select(job, snapshot);
      selected_policy = policy_;
      if (id) {
        std::lock_guard<std::mutex> state_lock(proxies_mu_);
        auto it = std::find_if(proxies_.begin(), proxies_.end(), [id](const auto& p) { return p.id == *id; });
        if (it != proxies_.end() && (!health_aware_ || it->healthy) && it->circuit_state == CircuitState::Closed) {
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
    metrics_.inc(selection_counter(selected_policy));
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
    upstream_ms += result.latency_ms;
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
