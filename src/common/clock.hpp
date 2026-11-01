#pragma once
// Monotonic clock behind a tiny interface so tests can control time.
#include <chrono>

namespace pg {

using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::steady_clock::duration;

class Clock {
 public:
  virtual ~Clock() = default;
  virtual TimePoint now() const = 0;
};

class SteadyClock final : public Clock {
 public:
  TimePoint now() const override { return std::chrono::steady_clock::now(); }
};

// Test clock. Not thread-safe: advance it from the test thread only.
class FakeClock final : public Clock {
 public:
  TimePoint now() const override { return t_; }
  void advance(Duration d) { t_ += d; }

 private:
  TimePoint t_{};
};

}  // namespace pg
