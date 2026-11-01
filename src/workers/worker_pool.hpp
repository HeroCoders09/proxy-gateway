#pragma once
#include <condition_variable>
#include <deque>
#include <thread>
#include "common/config.hpp"
#include "common/fd.hpp"
#include "common/metrics.hpp"
#include "gateway/gateway_core.hpp"
#include "workers/aimd.hpp"
namespace pg { class WorkerPool { public: WorkerPool(const GatewayConfig&,GatewayCore&,MetricsSink&);~WorkerPool();void start();void stop();void submit(Job,Fd,bool);int thread_count()const;int active_jobs()const;int aimd_limit()const;AimdController& aimd(){return aimd_;} private:struct Task{Job job;Fd client;bool head;};void loop();const GatewayConfig&cfg_;GatewayCore&core_;MetricsSink&metrics_;AimdController aimd_;mutable std::mutex mu_;std::condition_variable cv_;std::deque<Task>q_;bool stopping_=false;int active_=0;std::vector<std::thread>threads_;std::thread control_;}; }
