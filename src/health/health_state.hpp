#pragma once

#include <deque>
#include <utility>

#include "common/clock.hpp"
#include "common/types.hpp"

namespace pg {

class RollingStats {
 public:
  explicit RollingStats(size_t capacity = 20) : capacity_(capacity ? capacity : 1) {}
  void add(bool success, double rtt_ms);
  size_t size() const { return samples_.size(); }
  double success_rate() const;
  double recent_success_rate(size_t count) const;
  double mean_rtt_ms() const;

 private:
  size_t capacity_;
  std::deque<std::pair<bool, double>> samples_;
};

class CircuitBreaker {
 public:
  CircuitBreaker(int threshold, Duration cooldown, const Clock& clock)
      : threshold_(threshold), cooldown_(cooldown), clock_(clock) {}
  void record(bool success);
  bool begin_probe();
  void finish_probe(bool success);
  CircuitState state() const { return state_; }
  uint32_t consecutive_failures() const { return consecutive_failures_; }
  uint64_t open_count() const { return open_count_; }
  bool probe_in_flight() const { return probe_in_flight_; }

 private:
  void open();
  int threshold_;
  Duration cooldown_;
  const Clock& clock_;
  CircuitState state_ = CircuitState::Closed;
  uint32_t consecutive_failures_ = 0;
  uint64_t open_count_ = 0;
  TimePoint opened_at_{};
  bool probe_in_flight_ = false;
};

}  // namespace pg
