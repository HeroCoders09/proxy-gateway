#include "health/health_state.hpp"

#include <algorithm>

namespace pg {

void RollingStats::add(bool success, double rtt_ms) {
  samples_.emplace_back(success, rtt_ms);
  while (samples_.size() > capacity_) samples_.pop_front();
}

double RollingStats::success_rate() const {
  return recent_success_rate(samples_.size());
}

double RollingStats::recent_success_rate(size_t count) const {
  if (samples_.empty() || count == 0) return 1.0;
  count = std::min(count, samples_.size());
  const auto begin = samples_.end() - static_cast<std::ptrdiff_t>(count);
  const size_t successes = static_cast<size_t>(std::count_if(begin, samples_.end(),
      [](const auto& sample) { return sample.first; }));
  return static_cast<double>(successes) / static_cast<double>(count);
}

double RollingStats::mean_rtt_ms() const {
  double total = 0.0;
  size_t count = 0;
  for (const auto& [success, rtt] : samples_) {
    (void)success;
    if (rtt > 0.0) { total += rtt; ++count; }
  }
  return count ? total / static_cast<double>(count) : 0.0;
}

void CircuitBreaker::open() {
  state_ = CircuitState::Open;
  opened_at_ = clock_.now();
  probe_in_flight_ = false;
  ++open_count_;
}

void CircuitBreaker::record(bool success) {
  if (state_ == CircuitState::Open || state_ == CircuitState::HalfOpen) return;
  if (success) consecutive_failures_ = 0;
  else if (++consecutive_failures_ >= static_cast<uint32_t>(threshold_)) open();
}

bool CircuitBreaker::begin_probe() {
  if (state_ == CircuitState::Open) {
    if (clock_.now() - opened_at_ < cooldown_) return false;
    state_ = CircuitState::HalfOpen;
  }
  if (state_ == CircuitState::HalfOpen && probe_in_flight_) return false;
  probe_in_flight_ = true;
  return true;
}

void CircuitBreaker::finish_probe(bool success) {
  if (!probe_in_flight_) return;
  probe_in_flight_ = false;
  if (state_ == CircuitState::HalfOpen) {
    if (success) {
      state_ = CircuitState::Closed;
      consecutive_failures_ = 0;
    } else {
      consecutive_failures_ = 0;
      open();
    }
    return;
  }
  if (success) consecutive_failures_ = 0;
  else if (++consecutive_failures_ >= static_cast<uint32_t>(threshold_)) open();
}

}  // namespace pg
