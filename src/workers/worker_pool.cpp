#include "workers/worker_pool.hpp"
#include "ingress/http_response.hpp"
namespace pg {
WorkerPool::WorkerPool(const GatewayConfig& c, GatewayCore& g, MetricsSink& m, ResultWriter* writer)
    : cfg_(c), core_(g), metrics_(m), result_writer_(writer), aimd_(c.workers) {}
WorkerPool::~WorkerPool() { stop(); }
void WorkerPool::start() {
  std::lock_guard<std::mutex> l(mu_);
  if (!threads_.empty()) return;
  stopping_ = false;
  for (int i = 0; i < cfg_.workers.max; ++i) threads_.emplace_back([this] { loop(); });
  control_ = std::thread([this] {
    for (;;) {
      { std::lock_guard<std::mutex> l(mu_); if (stopping_) return; }
      std::this_thread::sleep_for(std::chrono::seconds(1));
      aimd_.evaluate();
      cv_.notify_all();
    }
  });
}
void WorkerPool::stop() {
  { std::lock_guard<std::mutex> l(mu_); stopping_ = true; }
  cv_.notify_all();
  for (auto& t : threads_) if (t.joinable()) t.join();
  threads_.clear();
  if (control_.joinable()) control_.join();
}
void WorkerPool::submit(Job j, Fd c, bool h) {
  { std::lock_guard<std::mutex> l(mu_); q_.push_back({std::move(j), std::move(c), h}); }
  cv_.notify_all();
}
int WorkerPool::thread_count() const { std::lock_guard<std::mutex> l(mu_); return static_cast<int>(threads_.size()); }
int WorkerPool::active_jobs() const { std::lock_guard<std::mutex> l(mu_); return active_; }
int WorkerPool::aimd_limit() const { return aimd_.limit(); }
void WorkerPool::loop() {
  for (;;) {
    Task t;
    {
      std::unique_lock<std::mutex> l(mu_);
      cv_.wait(l, [this] { return stopping_ || (!q_.empty() && active_ < aimd_.limit()); });
      if (stopping_) return;
      t = std::move(q_.front()); q_.pop_front(); ++active_;
    }
    const auto started = std::chrono::steady_clock::now();
    HttpResponse r;
    bool failed = false;
    try { r = core_.handle(t.job); failed = r.status >= 500; }
    catch (...) { r = make_error_response(502, "internal gateway error"); failed = true; }
    write_response(t.client.get(), r, t.head, 5000);
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (result_writer_) {
      int proxy = -1;
      for (const auto& h : r.headers) if (h.name == "X-Gateway-Proxy") {
        try { proxy = std::stoi(h.value); } catch (...) { proxy = -1; }
      }
      const auto now = std::chrono::system_clock::now().time_since_epoch();
      result_writer_->record({std::chrono::duration_cast<std::chrono::milliseconds>(now).count(),
                              safe_result_url(t.job.url), r.status, proxy, elapsed, !failed});
    }
    aimd_.record(failed);
    { std::lock_guard<std::mutex> l(mu_); --active_; }
    cv_.notify_all();
  }
}
}  // namespace pg
