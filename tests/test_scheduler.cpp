#include "catch_amalgamated.hpp"
#include <chrono>
#include <thread>
#include "common/config.hpp"
#include "common/metrics.hpp"
#include "scheduler/domain_scheduler.hpp"
#include "workers/aimd.hpp"
using namespace pg;

static GatewayConfig schedule_cfg() {
  return parse_config_text(R"({"proxies":[{"host":"127.0.0.1","port":3128}],"queue_limit":2,
    "rate_limits":{"default":{"rate":2,"burst":2},"site-a.example":{"rate":1,"burst":1}},
    "workers":{"min":2,"max":8,"alpha":1,"beta":0.5}})");
}
TEST_CASE("token buckets burst refill cap and domain isolation") {
  GatewayConfig cfg=schedule_cfg(); Metrics m; FakeClock clock;
  DomainScheduler s(cfg,m,clock,[](Job,Fd,bool){});
  REQUIRE(s.try_acquire("site-a.example"));
  REQUIRE_FALSE(s.try_acquire("site-a.example"));
  REQUIRE(s.try_acquire("site-b.example"));
  REQUIRE(s.try_acquire("site-b.example"));
  REQUIRE_FALSE(s.try_acquire("site-b.example"));
  clock.advance(std::chrono::seconds(1));
  REQUIRE(s.try_acquire("site-a.example"));
  REQUIRE(s.try_acquire("site-b.example"));
  clock.advance(std::chrono::seconds(10));
  REQUIRE(s.try_acquire("site-a.example"));
  REQUIRE_FALSE(s.try_acquire("site-a.example"));
}
TEST_CASE("domain queue rejects overflow and releases FIFO") {
  GatewayConfig cfg=schedule_cfg(); cfg.queue_limit=1; cfg.default_rate={1000,1000}; Metrics m; SteadyClock clock;
  std::vector<uint64_t> seen; std::mutex mu;
  DomainScheduler s(cfg,m,clock,[&](Job j,Fd,bool){std::lock_guard<std::mutex>l(mu);seen.push_back(j.id);});
  Fd a,b; Job one;one.id=1;one.domain="x";Job two;two.id=2;two.domain="x";
  REQUIRE(s.enqueue(std::move(one),a,false));
  REQUIRE_FALSE(s.enqueue(std::move(two),b,false));
  REQUIRE(m.get(Counter::QueueRejected)==1);
  s.start(); std::this_thread::sleep_for(std::chrono::milliseconds(30)); s.stop();
  REQUIRE(seen==std::vector<uint64_t>{1});
}
TEST_CASE("AIMD increases on healthy windows and decreases on error pressure") {
  WorkersCfg c; c.min=2;c.max=8;c.alpha=1;c.beta=0.5; AimdController a(c);
  REQUIRE(a.limit()==2); a.record(false);a.evaluate();REQUIRE(a.limit()==3);
  for (int i=0; i<10; ++i) a.record(i<2);
  a.evaluate();
  REQUIRE(a.limit()==2);
  for (int i=0; i<10; ++i) a.record(false);
  a.evaluate();
  REQUIRE(a.limit()==3);
}
