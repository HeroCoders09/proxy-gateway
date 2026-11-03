#pragma once

#include "types.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>

namespace gateway {

// Additive-increase / multiplicative-decrease concurrency limit, evaluated every 20 completed requests.
class AIMDController {
public:
    explicit AIMDController(const GatewayConfig& cfg)
        : min_c_(cfg.workers_min),
          max_c_(cfg.workers_max),
          alpha_(cfg.aimd_alpha),
          beta_(cfg.aimd_beta),
          current_c_(cfg.workers_initial) {}

    // Called when the limit changes so that waiting workers can re-check it.
    void set_on_change(std::function<void()> cb) { on_change_ = std::move(cb); }

    void record_feedback(bool success, bool timeout_or_error) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            window_total_++;
            if (timeout_or_error || !success) window_errors_++;
            if (window_total_ >= 20) {
                double err_ratio = static_cast<double>(window_errors_) / window_total_;
                int c = current_c_.load();
                int next_c;
                if (err_ratio >= 0.10) {
                    next_c = std::max(static_cast<int>(std::floor(beta_ * c)), min_c_);  // multiplicative decrease
                } else {
                    next_c = std::min(c + alpha_, max_c_);                              // additive increase
                }
                changed = (next_c != c);
                current_c_.store(next_c);
                window_total_ = 0;
                window_errors_ = 0;
            }
        }
        if (changed && on_change_) on_change_();
    }

    int limit() const { return current_c_.load(std::memory_order_relaxed); }

private:
    int min_c_;
    int max_c_;
    int alpha_;
    double beta_;
    std::atomic<int> current_c_;
    std::mutex mtx_;
    int window_total_ = 0;
    int window_errors_ = 0;
    std::function<void()> on_change_;
};

} // namespace gateway
