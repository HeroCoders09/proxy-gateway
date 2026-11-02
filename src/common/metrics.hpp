#pragma once
// Shared Metrics store and the MetricsSink interface: the ONLY way modules write measurements.
// Each phase adds its own counters/gauges here (handbook rule 7).
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace pg {

enum class Counter : size_t {
  RequestsTotal = 0,      // well-formed requests accepted by ingress
  RequestsSuccessful,     // transport OK and target status < 500
  RequestsFailed,         // transport error, or target/proxy status >= 500
  Timeouts,               // connect or response timeouts (also counted in RequestsFailed)
  BadRequests,            // malformed requests answered with 400
  QueueRejected,          // domain queue was full; answered with 503
  Retries,                // additional attempts after the first attempt
  NoRoute,                // requests for which no eligible proxy was available
  RandomSelections,
  RoundRobinSelections,
  LeastConnSelections,
  LatencySelections,
  WeightedSelections,
  ProbeSuccesses,
  ProbeFailures,
  ConnectionsOpened,
  ConnectionsReused,
  ConnectTimeouts,
  ResponseTimeouts,
  BytesIn,
  BytesOut,
  OutputRowsDropped,
  kCount
};

enum class Gauge : size_t { UpstreamActiveConnections = 0, UpstreamIdleConnections, kCount };

class MetricsSink {
 public:
  virtual ~MetricsSink() = default;
  virtual void inc(Counter c, uint64_t n = 1) = 0;
  virtual void add_gauge(Gauge g, int64_t delta) = 0;
  virtual void observe_latency_ms(double ms) = 0;
  virtual void observe_gateway_overhead_ms(double ms) = 0;
};

struct LatencyPercentiles {
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  size_t samples = 0;
};

class Metrics final : public MetricsSink {
 public:
  static constexpr size_t kLatencyRing = 4096;  // bounded sample window

  void inc(Counter c, uint64_t n = 1) override;
  void add_gauge(Gauge g, int64_t delta) override;
  void observe_latency_ms(double ms) override;
  void observe_gateway_overhead_ms(double ms) override;

  uint64_t get(Counter c) const;
  int64_t get(Gauge g) const;
  // Nearest-rank percentiles over the most recent kLatencyRing samples.
  LatencyPercentiles latency_percentiles() const;
  LatencyPercentiles gateway_overhead_percentiles() const;

 private:
  std::array<std::atomic<uint64_t>, static_cast<size_t>(Counter::kCount)> counters_{};
  std::array<std::atomic<int64_t>, static_cast<size_t>(Gauge::kCount)> gauges_{};
  mutable std::mutex mu_;
  std::vector<double> ring_;
  size_t next_ = 0;
  std::vector<double> overhead_ring_;
  size_t overhead_next_ = 0;
};

}  // namespace pg
