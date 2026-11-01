#pragma once
// Client-facing TCP listener (default port 8000). One accept thread feeds a bounded queue that a
// FIXED pool of handler threads drains: no thread is ever created per request.
//
// Phase 1 flow: parse request -> Job -> handler(Job) -> write the HttpResponse -> close.
// Phase 2 replaces the synchronous handler with enqueue-to-scheduler (see HANDOFF_PHASE_1.md).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "common/config.hpp"
#include "common/fd.hpp"
#include "common/metrics.hpp"
#include "ingress/http_response.hpp"

namespace pg {

class Listener {
 public:
  // The handler is given the parsed Job and returns the response to send to the client.
  // It may throw; the listener answers 502 and keeps running.
  using Handler = std::function<HttpResponse(Job&)>;
  // Phase 2 ownership handoff: the callback owns client and writes the reply later.
  using AsyncHandler = std::function<void(Job, Fd, bool)>;

  Listener(const GatewayConfig& cfg, MetricsSink& metrics, Handler handler);
  Listener(const GatewayConfig& cfg, MetricsSink& metrics, AsyncHandler handler);
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  // Bind, listen and start threads. Throws std::runtime_error if the socket cannot be set up.
  void start();
  void stop();

  // The bound port (useful when configured with port 0 in tests).
  uint16_t port() const { return bound_port_; }

 private:
  void accept_loop();
  void worker_loop();
  void handle_connection(Fd client);

  const GatewayConfig cfg_;
  MetricsSink& metrics_;
  Handler handler_;
  AsyncHandler async_handler_;

  Fd listen_fd_;
  uint16_t bound_port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> next_job_id_{1};

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Fd> pending_;  // bounded by cfg_.ingress.pending_connections
  std::thread accept_thread_;
  std::vector<std::thread> workers_;
};

}  // namespace pg
