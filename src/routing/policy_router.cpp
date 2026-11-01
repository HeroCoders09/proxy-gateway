#include "routing/policy_router.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <unordered_set>

namespace pg {

PolicyRouter::PolicyRouter(std::string policy, WeightsCfg weights)
    : policy_(std::move(policy)), weights_(weights), rng_(std::random_device{}()) {}

std::optional<ProxyId> PolicyRouter::select(const Job& job, const std::vector<ProxyState>& table) {
  std::unordered_set<ProxyId> tried(job.tried_proxies.begin(), job.tried_proxies.end());
  std::vector<const ProxyState*> eligible;
  eligible.reserve(table.size());
  for (const auto& proxy : table) {
    if (proxy.healthy && proxy.circuit_state == CircuitState::Closed && !tried.contains(proxy.id))
      eligible.push_back(&proxy);
  }
  if (eligible.empty()) return std::nullopt;

  std::lock_guard<std::mutex> lock(mu_);
  if (policy_ == "random") {
    std::uniform_int_distribution<size_t> pick(0, eligible.size() - 1);
    return eligible[pick(rng_)]->id;
  }
  if (policy_ == "roundrobin") {
    const auto* selected = eligible[rr_index_ % eligible.size()];
    ++rr_index_;
    return selected->id;
  }
  if (policy_ == "leastconn") {
    return (*std::min_element(eligible.begin(), eligible.end(), [](auto* a, auto* b) {
      return a->active_connections != b->active_connections ? a->active_connections < b->active_connections : a->id < b->id;
    }))->id;
  }
  if (policy_ == "latency") {
    std::vector<const ProxyState*> unmeasured;
    std::copy_if(eligible.begin(), eligible.end(), std::back_inserter(unmeasured), [](const auto* proxy) {
      return proxy->rtt_ms <= 0.0;
    });
    if (!unmeasured.empty()) {
      const auto* selected = unmeasured[rr_index_ % unmeasured.size()];
      ++rr_index_;
      return selected->id;
    }
    return (*std::min_element(eligible.begin(), eligible.end(), [](auto* a, auto* b) {
      return a->rtt_ms != b->rtt_ms ? a->rtt_ms < b->rtt_ms : a->id < b->id;
    }))->id;
  }

  double max_rtt = 0.0;
  int max_load = 0;
  for (const auto* proxy : eligible) {
    max_rtt = std::max(max_rtt, proxy->rtt_ms);
    max_load = std::max(max_load, proxy->active_connections);
  }
  const ProxyState* best = nullptr;
  double best_score = std::numeric_limits<double>::infinity();
  for (const auto* proxy : eligible) {
    const double latency = max_rtt > 0.0 ? proxy->rtt_ms / max_rtt : 0.5;
    const double load = max_load > 0 ? static_cast<double>(proxy->active_connections) / max_load : 0.5;
    const uint64_t attempts = proxy->success_count + proxy->failure_count;
    const double failure = attempts ? static_cast<double>(proxy->failure_count) / attempts : 0.5;
    const double score = weights_.latency * latency + weights_.load * load + weights_.failure * failure;
    if (!best || score < best_score || (score == best_score && proxy->id < best->id)) {
      best = proxy;
      best_score = score;
    }
  }
  return best ? std::optional<ProxyId>(best->id) : std::nullopt;
}

}  // namespace pg
