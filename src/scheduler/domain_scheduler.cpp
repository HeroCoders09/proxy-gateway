#include "scheduler/domain_scheduler.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
namespace pg {
DomainScheduler::DomainScheduler(const GatewayConfig& c, MetricsSink& m, Clock& k, Ready r):cfg_(c),metrics_(m),clock_(k),ready_(std::move(r)),default_rate_(c.default_rate){}
DomainScheduler::~DomainScheduler(){stop();}
RateLimitCfg DomainScheduler::rate_for(const std::string& d)const{auto i=cfg_.domain_rates.find(d);return i==cfg_.domain_rates.end()?default_rate_:i->second;}
void DomainScheduler::refill(Queue&q,TimePoint n)const{double s=std::chrono::duration<double>(n-q.last).count();if(s>0){q.tokens=std::min(q.capacity,q.tokens+s*q.rate);q.last=n;}}
bool DomainScheduler::try_acquire(const std::string&d){std::lock_guard<std::mutex>l(mu_);auto[i,newq]=queues_.try_emplace(d);if(newq){auto r=rate_for(d);i->second={{},r.burst,r.rate,r.burst,clock_.now()};}refill(i->second,clock_.now());if(i->second.tokens<1)return false;--i->second.tokens;return true;}
void DomainScheduler::set_default_rate(double rate,double burst){if(!std::isfinite(rate)||!std::isfinite(burst)||rate<=0||burst<=0)return;std::lock_guard<std::mutex>l(mu_);const auto now=clock_.now();default_rate_={rate,burst};for(auto&[domain,q]:queues_){if(cfg_.domain_rates.find(domain)!=cfg_.domain_rates.end())continue;refill(q,now);q.rate=rate;q.capacity=burst;q.tokens=std::min(q.tokens,burst);}}
bool DomainScheduler::enqueue(Job j,Fd& c,bool h){std::lock_guard<std::mutex>l(mu_);auto[i,newq]=queues_.try_emplace(j.domain);if(newq){auto r=rate_for(j.domain);i->second={{},r.burst,r.rate,r.burst,clock_.now()};}if(i->second.jobs.size()>=cfg_.queue_limit){metrics_.inc(Counter::QueueRejected);return false;}i->second.jobs.push_back({std::move(j),std::move(c),h});cv_.notify_one();return true;}
std::map<std::string,size_t> DomainScheduler::queue_lengths()const{std::lock_guard<std::mutex>l(mu_);std::map<std::string,size_t>r;for(auto const&[d,q]:queues_)r[d]=q.jobs.size();return r;}
void DomainScheduler::start(){std::lock_guard<std::mutex>l(mu_);if(thread_.joinable())return;stopping_=false;thread_=std::thread([this]{loop();});}
void DomainScheduler::stop(){{std::lock_guard<std::mutex>l(mu_);stopping_=true;}cv_.notify_all();if(thread_.joinable())thread_.join();}
void DomainScheduler::loop(){for(;;){std::vector<Entry>ready;{std::unique_lock<std::mutex>l(mu_);cv_.wait_for(l,std::chrono::milliseconds(5),[this]{return stopping_||!queues_.empty();});if(stopping_)return;auto n=clock_.now();for(auto&[d,q]:queues_){refill(q,n);while(!q.jobs.empty()&&q.tokens>=1){--q.tokens;ready.push_back(std::move(q.jobs.front()));q.jobs.pop_front();}}}for(auto&e:ready)ready_(std::move(e.job),std::move(e.client),e.head);}}
}
