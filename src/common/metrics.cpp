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

void Metrics::add_gauge(Gauge g, int64_t delta) {
  gauges_[static_cast<size_t>(g)].fetch_add(delta, std::memory_order_relaxed);
}

int64_t Metrics::get(Gauge g) const {
  return gauges_[static_cast<size_t>(g)].load(std::memory_order_relaxed);
}

void Metrics::observe_latency_ms(double ms) {
  ring_.observe(ms);
}

void Metrics::observe_gateway_overhead_ms(double ms) {
  overhead_ring_.observe(ms);
}

void Metrics::SampleRing::observe(double ms) {
  const uint64_t ticket = next_.fetch_add(1, std::memory_order_relaxed);
  const size_t slot = static_cast<size_t>(ticket % kLatencyRing);
  std::lock_guard<std::mutex> lock(slot_mutexes_[slot]);
  if (ticket + 1 > sequence_[slot]) {
    values_[slot] = ms;
    sequence_[slot] = ticket + 1;
  }
}

std::vector<double> Metrics::SampleRing::snapshot() const {
  const uint64_t end = next_.load(std::memory_order_relaxed);
  const uint64_t begin = end > kLatencyRing ? end - kLatencyRing : 0;
  std::vector<double> result;
  result.reserve(static_cast<size_t>(end - begin));
  for (uint64_t ticket = begin; ticket < end; ++ticket) {
    const size_t slot = static_cast<size_t>(ticket % kLatencyRing);
    std::lock_guard<std::mutex> lock(slot_mutexes_[slot]);
    if (sequence_[slot] == ticket + 1) result.push_back(values_[slot]);
  }
  return result;
}

static double nearest_rank(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  size_t rank = static_cast<size_t>(std::ceil(p * static_cast<double>(sorted.size())));
  if (rank < 1) rank = 1;
  if (rank > sorted.size()) rank = sorted.size();
  return sorted[rank - 1];
}

LatencyPercentiles Metrics::latency_percentiles() const {
  std::vector<double> copy = ring_.snapshot();
  std::sort(copy.begin(), copy.end());
  LatencyPercentiles out;
  out.samples = copy.size();
  out.p50 = nearest_rank(copy, 0.50);
  out.p95 = nearest_rank(copy, 0.95);
  out.p99 = nearest_rank(copy, 0.99);
  return out;
}

LatencyPercentiles Metrics::gateway_overhead_percentiles() const {
  std::vector<double> copy = overhead_ring_.snapshot();
  std::sort(copy.begin(), copy.end());
  LatencyPercentiles out;
  out.samples = copy.size();
  out.p50 = nearest_rank(copy, 0.50);
  out.p95 = nearest_rank(copy, 0.95);
  out.p99 = nearest_rank(copy, 0.99);
  return out;
}

}  // namespace pg
