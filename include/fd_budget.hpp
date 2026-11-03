#pragma once
// File-descriptor budget: every client connection, in-flight upstream request, CONNECT tunnel (2 fds) and idle
// pooled upstream connection holds a descriptor. Clamp the configured limits to RLIMIT_NOFILE so the gateway
// degrades predictably (rejects / stops pooling) instead of running into EMFILE.
#include <algorithm>
#include <string>
#include <vector>

#include "types.hpp"

namespace gateway {

constexpr long kFdReserve = 64;   // listeners, epoll/event fds, log and result files, DNS, admin sockets

// Returns human-readable notes for every limit that had to be lowered (empty if the limits fit).
inline std::vector<std::string> apply_fd_budget(GatewayConfig& cfg, long fd_limit) {
    std::vector<std::string> notes;
    long usable = fd_limit - kFdReserve;
    if (usable < 32) {
        notes.push_back("RLIMIT_NOFILE is only " + std::to_string(fd_limit) + ": raise it (ulimit -n / LimitNOFILE) for real traffic");
        usable = 32;
    }
    // Idle pool: at most a quarter of the descriptors.
    size_t pool_cap = static_cast<size_t>(usable / 4);
    if (cfg.pool_max_idle_total > pool_cap) {
        notes.push_back("limits.pool_max_idle_total " + std::to_string(cfg.pool_max_idle_total) + " lowered to " +
                        std::to_string(pool_cap) + " (open-file limit " + std::to_string(fd_limit) + ")");
        cfg.pool_max_idle_total = pool_cap;
    }
    cfg.pool_max_idle_per_proxy = std::min(cfg.pool_max_idle_per_proxy, cfg.pool_max_idle_total);
    // Clients: each may hold one upstream connection (a tunnel holds two fds in total).
    size_t client_cap = static_cast<size_t>(std::max<long>(1, (usable - static_cast<long>(cfg.pool_max_idle_total)) / 2));
    if (cfg.max_client_connections > client_cap) {
        notes.push_back("limits.max_client_connections " + std::to_string(cfg.max_client_connections) + " lowered to " +
                        std::to_string(client_cap) + " (open-file limit " + std::to_string(fd_limit) + ")");
        cfg.max_client_connections = client_cap;
    }
    return notes;
}

}  // namespace gateway
