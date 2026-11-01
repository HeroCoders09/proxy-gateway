#include "workers/aimd.hpp"
#include <algorithm>
#include <cmath>
namespace pg { void AimdController::record(bool f){std::lock_guard<std::mutex>l(mu_);++total_;if(f)++failed_;}void AimdController::evaluate(){std::lock_guard<std::mutex>l(mu_);if(total_){if(failed_*10>=total_)limit_=std::max(cfg_.min,(int)std::floor(cfg_.beta*limit_));else limit_=std::min(cfg_.max,limit_+(int)std::ceil(cfg_.alpha));}total_=failed_=0;}int AimdController::limit()const{std::lock_guard<std::mutex>l(mu_);return limit_;} }
