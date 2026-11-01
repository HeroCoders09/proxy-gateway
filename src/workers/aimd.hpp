#pragma once
#include <mutex>
#include "common/config.hpp"
namespace pg { class AimdController { public: explicit AimdController(const WorkersCfg& c):cfg_(c),limit_(c.min){} void record(bool); void evaluate(); int limit()const; private: WorkersCfg cfg_;mutable std::mutex mu_;int limit_;unsigned total_=0,failed_=0; }; }
