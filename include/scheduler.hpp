#pragma once

#include "types.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gateway {

struct DomainQueue {
    std::string name;
    std::deque<Job> fifo;
    double tokens = 0.0;
    double refill_rate = 200.0;
    double burst_capacity = 400.0;
    TimePoint last_refill = Clock::now();
    TimePoint last_used = Clock::now();
    bool active = false;  // present in the scheduler's active list

    DomainQueue() = default;
    DomainQueue(std::string n, double r, double b)
        : name(std::move(n)), tokens(b), refill_rate(r), burst_capacity(b),
          last_refill(Clock::now()), last_used(Clock::now()) {}

    void refill(TimePoint now) {
        double elapsed = std::chrono::duration<double>(now - last_refill).count();
        if (elapsed > 0.0) {
            tokens = std::min(burst_capacity, tokens + elapsed * refill_rate);
            last_refill = now;
        }
    }
};

// Per-domain FIFO queues + token-bucket rate limiting. Workers block on a condition variable
// (no polling): they are woken by enqueue(), by token refill timers and by queue deadlines.
class JobScheduler {
public:
    explicit JobScheduler(const GatewayConfig& cfg)
        : max_queue_size_(cfg.max_queue_size),
          default_limit_(cfg.default_rate_limit),
          domain_limits_(cfg.domain_rate_limits) {}

    // FR-1: enqueue into the domain FIFO; false when the bounded queue is full (HTTP 503).
    bool enqueue(Job job) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (shutdown_ || total_queued_ >= max_queue_size_) return false;
            DomainQueue& dq = get_or_create_locked(job.domain);
            dq.last_used = Clock::now();
            dq.fifo.push_back(std::move(job));
            ++total_queued_;
            if (!dq.active) {
                dq.active = true;
                active_.push_back(&dq);
            }
        }
        cv_.notify_one();
        return true;
    }

    // FR-2 (non-blocking): release the oldest ready job of a domain that has >= 1 token.
    std::optional<Job> try_pop_ready_job() {
        std::lock_guard<std::mutex> lock(mtx_);
        return pop_ready_locked(Clock::now());
    }

    // Blocks until a job is ready, some queued jobs expired, or shutdown().
    // Expired jobs (queue deadline passed) are appended to `expired`; the caller must answer them.
    // Returns nullopt when only expired jobs were produced, or on shutdown.
    std::optional<Job> pop_blocking(std::vector<Job>& expired) {
        std::unique_lock<std::mutex> lock(mtx_);
        for (;;) {
            if (shutdown_) return std::nullopt;
            TimePoint now = Clock::now();
            collect_expired_locked(now, expired);
            std::optional<Job> job = pop_ready_locked(now);
            prune_idle_locked(now);
            if (job || !expired.empty()) {
                if (total_queued_ > 0) cv_.notify_one();  // chain-wake another worker
                return job;
            }
            TimePoint wake = TimePoint::max();
            for (DomainQueue* dq : active_) {
                if (dq->fifo.empty()) continue;
                dq->refill(now);
                if (dq->tokens < 1.0) {
                    double need = (1.0 - dq->tokens) / dq->refill_rate;
                    auto t = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(need)) +
                             std::chrono::microseconds(50);
                    wake = std::min(wake, t);
                }
                wake = std::min(wake, dq->fifo.front().deadline);
            }
            if (wake == TimePoint::max()) cv_.wait(lock);
            else cv_.wait_until(lock, wake);
        }
    }

    // Moves jobs whose queue deadline has passed into `out` (the caller must answer them).
    void expire(std::vector<Job>& out) {
        std::lock_guard<std::mutex> lock(mtx_);
        collect_expired_locked(Clock::now(), out);
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    size_t total_queued() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return total_queued_;
    }

private:
    DomainQueue& get_or_create_locked(const std::string& domain) {
        auto it = queues_.find(domain);
        if (it == queues_.end()) {
            RateLimitConfig rl = default_limit_;
            auto cfg_it = domain_limits_.find(domain);
            if (cfg_it != domain_limits_.end()) rl = cfg_it->second;
            it = queues_.emplace(domain, DomainQueue(domain, rl.rate, rl.burst)).first;
        }
        return it->second;
    }

    std::optional<Job> pop_ready_locked(TimePoint now) {
        for (auto it = active_.begin(); it != active_.end(); ++it) {
            DomainQueue* dq = *it;
            if (dq->fifo.empty()) continue;
            dq->refill(now);
            if (dq->tokens < 1.0) continue;
            dq->tokens -= 1.0;
            Job ready = std::move(dq->fifo.front());
            dq->fifo.pop_front();
            --total_queued_;
            dq->last_used = now;
            if (dq->fifo.empty()) {
                dq->active = false;
                active_.erase(it);
            } else {
                active_.splice(active_.end(), active_, it);  // round-robin fairness between domains
            }
            return ready;
        }
        return std::nullopt;
    }

    void collect_expired_locked(TimePoint now, std::vector<Job>& expired) {
        for (auto it = active_.begin(); it != active_.end();) {
            DomainQueue* dq = *it;
            while (!dq->fifo.empty() && dq->fifo.front().deadline <= now) {
                expired.push_back(std::move(dq->fifo.front()));
                dq->fifo.pop_front();
                --total_queued_;
            }
            if (dq->fifo.empty()) {
                dq->active = false;
                it = active_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Drop per-domain state of domains that have been idle for a minute (bounded memory).
    void prune_idle_locked(TimePoint now) {
        if (now - last_prune_ < std::chrono::seconds(10)) return;
        last_prune_ = now;
        for (auto it = queues_.begin(); it != queues_.end();) {
            if (!it->second.active && it->second.fifo.empty() &&
                now - it->second.last_used > std::chrono::seconds(60)) {
                it = queues_.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t max_queue_size_;
    RateLimitConfig default_limit_;
    std::unordered_map<std::string, RateLimitConfig> domain_limits_;

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::unordered_map<std::string, DomainQueue> queues_;  // node-based: pointers stay valid
    std::list<DomainQueue*> active_;                       // domains with queued jobs, round-robin order
    size_t total_queued_ = 0;
    bool shutdown_ = false;
    TimePoint last_prune_ = Clock::now();
};

} // namespace gateway
