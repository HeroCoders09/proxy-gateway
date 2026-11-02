#pragma once

#include <memory>

#include "common/interfaces.hpp"

namespace pg {

// Single level-triggered epoll reactor. Calls to forward() wait on their own result while the
// reactor owns all upstream descriptors and can advance many sockets concurrently.
class EpollForwarder final : public Forwarder {
 public:
  EpollForwarder(int connect_timeout_ms, int response_timeout_ms,
                 size_t max_response_bytes = 16u * 1024u * 1024u,
                 size_t max_connections_per_proxy = 8, int idle_timeout_ms = 30000,
                 MetricsSink* metrics = nullptr, size_t max_total_connections = 128);
  ~EpollForwarder() override;

  EpollForwarder(const EpollForwarder&) = delete;
  EpollForwarder& operator=(const EpollForwarder&) = delete;

  ForwardResult forward(const Job& job, const ProxyState& proxy) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pg
