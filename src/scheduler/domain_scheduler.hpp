#pragma once
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include "common/config.hpp"
#include "common/interfaces.hpp"
#include "common/metrics.hpp"
namespace pg {
class DomainScheduler final : public RateLimiter {
 public:
  using Ready = std::function<void(Job, Fd, bool)>;
  DomainScheduler(const GatewayConfig&, MetricsSink&, Clock&, Ready);
  ~DomainScheduler();
  void start(); void stop(); bool enqueue(Job, Fd&, bool); bool try_acquire(const std::string&) override;
  void set_default_rate(double rate, double burst);
  std::map<std::string, size_t> queue_lengths() const;
 private:
  struct Entry { Job job; Fd client; bool head = false; };
  struct Queue { std::deque<Entry> jobs; double tokens, rate, capacity; TimePoint last; };
  void loop(); void refill(Queue&, TimePoint) const; RateLimitCfg rate_for(const std::string&) const;
  const GatewayConfig& cfg_; MetricsSink& metrics_; Clock& clock_; Ready ready_;
  mutable std::mutex mu_; std::condition_variable cv_; std::map<std::string, Queue> queues_;
  RateLimitCfg default_rate_;
  bool stopping_ = false; std::thread thread_;
};
}
