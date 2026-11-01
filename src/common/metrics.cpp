#include "common/metrics.hpp"

#include <algorithm>
#include <cmath>

namespace pg {

void Metrics::inc(Counter c, uint64_t n) {
  counters_[static_cast<size_t>(c)].fetch_add(n, std::memory_order_relaxed);
}

uint64_t Metrics::get(Counter c) const {
  return counters_[static_cast<size_t>(c)].load(std::memory_order_relaxed);
}

void Metrics::observe_latency_ms(double ms) {
  std::lock_guard<std::mutex> lk(mu_);
  if (ring_.size() < kLatencyRing) {
    ring_.push_back(ms);
  } else {
    ring_[next_] = ms;
    next_ = (next_ + 1) % kLatencyRing;
  }
}

static double nearest_rank(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  size_t rank = static_cast<size_t>(std::ceil(p * static_cast<double>(sorted.size())));
  if (rank < 1) rank = 1;
  if (rank > sorted.size()) rank = sorted.size();
  return sorted[rank - 1];
}

LatencyPercentiles Metrics::latency_percentiles() const {
  std::vector<double> copy;
  {
    std::lock_guard<std::mutex> lk(mu_);
    copy = ring_;
  }
  std::sort(copy.begin(), copy.end());
  LatencyPercentiles out;
  out.samples = copy.size();
  out.p50 = nearest_rank(copy, 0.50);
  out.p95 = nearest_rank(copy, 0.95);
  out.p99 = nearest_rank(copy, 0.99);
  return out;
}

}  // namespace pg
