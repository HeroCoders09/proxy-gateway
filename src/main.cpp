// Intelligent Concurrent Proxy Gateway entry point.
#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "admin/admin_server.hpp"
#include "common/config.hpp"
#include "common/log.hpp"
#include "common/metrics.hpp"
#include "gateway/gateway_core.hpp"
#include "health/health_monitor.hpp"
#include "ingress/listener.hpp"
#include "net/epoll_forwarder.hpp"
#include "net/simple_forwarder.hpp"
#include "output/result_writer.hpp"
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
  ResultWriter result_writer(cfg.output, metrics);
  std::unique_ptr<Forwarder> forwarder;
  if (cfg.forwarder == "epoll")
    forwarder = std::make_unique<EpollForwarder>(cfg.connect_timeout_ms, cfg.response_timeout_ms,
                                                16u * 1024u * 1024u, cfg.forwarder_pool.max_connections_per_proxy,
                                                cfg.forwarder_pool.idle_timeout_ms, &metrics,
                                                cfg.forwarder_pool.max_total_connections);
  else
    forwarder = std::make_unique<SimpleForwarder>(cfg.connect_timeout_ms, cfg.response_timeout_ms);
  GatewayCore core(cfg, *forwarder, metrics);
  HealthMonitor health(cfg, core);
  SteadyClock clock;
  WorkerPool pool(cfg, core, metrics, &result_writer);
  DomainScheduler scheduler(cfg, metrics, clock, [&pool](Job job, Fd client, bool head_only) {
    pool.submit(std::move(job), std::move(client), head_only);
  });
  AdminServer admin(cfg, metrics, [&scheduler, &pool] {
    return Phase2Stats{scheduler.queue_lengths(), pool.thread_count(), pool.aimd_limit(), pool.active_jobs()};
  }, [&core] { return core.proxies(); }, [&result_writer] { return result_writer.recent(); },
     [&core] { return core.policy(); }, [&core](const std::string& p) { return core.set_policy(p); },
     [&scheduler](double rate, double burst) { scheduler.set_default_rate(rate, burst); },
     [&core] { return core.health_aware(); }, [&core](bool enabled) { core.set_health_aware(enabled); });
  Listener listener(cfg, metrics, [&scheduler, &result_writer](Job job, Fd client, bool head_only) {
    const std::string safe_url = safe_result_url(job.url);
    if (!scheduler.enqueue(std::move(job), client, head_only)) {
      auto started = std::chrono::steady_clock::now();
      HttpResponse response = make_error_response(503, "domain queue full");
      write_response(client.get(), response, head_only, 5000);
      const auto now = std::chrono::system_clock::now().time_since_epoch();
      const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      result_writer.record({std::chrono::duration_cast<std::chrono::milliseconds>(now).count(),
                            safe_url, response.status, -1, elapsed, false});
    }
  });

  try {
    result_writer.start();
    pool.start();
    scheduler.start();
    admin.start();
    listener.start();
    health.start();
  } catch (const std::exception& e) {
    log_error(std::string("startup failed: ") + e.what());
    return 1;
  }

  log_info("gateway listening on " + cfg.listen_host + ":" + std::to_string(listener.port()) + ", admin on " +
           cfg.admin_host + ":" + std::to_string(admin.port()));
  log_info("routing via " + std::to_string(cfg.proxies.size()) + " configured proxies using policy '" + cfg.policy + "'");

  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

  log_info("shutting down");
  listener.stop();
  health.stop();
  admin.stop();
  scheduler.stop();
  pool.stop();
  result_writer.stop();
  return 0;
}
