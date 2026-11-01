// Intelligent Concurrent Proxy Gateway: entry point (Phase 1: foundation and skeleton).
#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "admin/admin_server.hpp"
#include "common/config.hpp"
#include "common/log.hpp"
#include "common/metrics.hpp"
#include "gateway/gateway_core.hpp"
#include "ingress/listener.hpp"
#include "net/simple_forwarder.hpp"
#include "scheduler/domain_scheduler.hpp"
#include "workers/worker_pool.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
  using namespace pg;

  GatewayConfig cfg;
  try {
    CliOptions opts = parse_cli(argc, argv);
    if (opts.help) {
      std::fputs(usage_text().c_str(), stdout);
      return 0;
    }
    if (opts.config_path.empty()) throw ConfigError("--config FILE is required (try --help)");
    cfg = load_config_file(opts.config_path);
    apply_cli_overrides(cfg, opts);
    if (cfg.forwarder != "simple") {
      throw ConfigError("forwarder '" + cfg.forwarder + "' is not implemented yet (epoll arrives in Phase 5); use 'simple'");
    }
  } catch (const ConfigError& e) {
    std::fprintf(stderr, "gateway: error: %s\n", e.what());
    return 2;
  }

  // A client closing its socket must never kill the process.
  ::signal(SIGPIPE, SIG_IGN);
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);

  Metrics metrics;
  SimpleForwarder forwarder(cfg.connect_timeout_ms, cfg.response_timeout_ms);
  GatewayCore core(cfg, forwarder, metrics);
  SteadyClock clock;
  WorkerPool pool(cfg, core, metrics);
  DomainScheduler scheduler(cfg, metrics, clock, [&pool](Job job, Fd client, bool head_only) {
    pool.submit(std::move(job), std::move(client), head_only);
  });
  AdminServer admin(cfg, metrics, [&scheduler, &pool] {
    return Phase2Stats{scheduler.queue_lengths(), pool.thread_count(), pool.aimd_limit(), pool.active_jobs()};
  });
  Listener listener(cfg, metrics, [&scheduler](Job job, Fd client, bool head_only) {
    if (!scheduler.enqueue(std::move(job), client, head_only))
      write_response(client.get(), make_error_response(503, "domain queue full"), false, 5000);
  });

  try {
    pool.start();
    scheduler.start();
    admin.start();
    listener.start();
  } catch (const std::exception& e) {
    log_error(std::string("startup failed: ") + e.what());
    return 1;
  }

  log_info("gateway listening on " + cfg.listen_host + ":" + std::to_string(listener.port()) + ", admin (stub) on " +
           cfg.admin_host + ":" + std::to_string(admin.port()));
  log_info("forwarding via proxy " + cfg.proxies.front().host + ":" + std::to_string(cfg.proxies.front().port) +
           " (policy '" + cfg.policy + "' is recorded but routing starts in Phase 3)");

  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

  log_info("shutting down");
  listener.stop();
  admin.stop();
  scheduler.stop();
  pool.stop();
  return 0;
}
