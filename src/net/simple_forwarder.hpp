#pragma once
// Phase 1 forwarder: plain blocking sockets (connect and I/O bounded by poll() deadlines) to ONE
// proxy, one connection per request ("Connection: close"). Kept in the tree as the correctness
// reference and fallback once the Phase 5 epoll forwarder exists.
#include "common/interfaces.hpp"

namespace pg {

class SimpleForwarder final : public Forwarder {
 public:
  SimpleForwarder(int connect_timeout_ms, int response_timeout_ms,
                  size_t max_response_bytes = 16u * 1024u * 1024u);

  ForwardResult forward(const Job& job, const ProxyState& proxy) override;

 private:
  int connect_timeout_ms_;
  int response_timeout_ms_;
  size_t max_response_bytes_;
};

// Builds the forward-proxy request text ("GET http://host/path HTTP/1.1 ..."). Exposed for tests.
std::string build_proxy_request(const Job& job, bool keep_alive = false,
                               const std::string& auth_username = {}, const std::string& auth_password = {});

}  // namespace pg
