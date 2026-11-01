#pragma once

#include <mutex>
#include <random>
#include <string>

#include "common/config.hpp"
#include "common/interfaces.hpp"

namespace pg {

class PolicyRouter final : public Router {
 public:
  PolicyRouter(std::string policy, WeightsCfg weights);
  std::optional<ProxyId> select(const Job& job, const std::vector<ProxyState>& table) override;

 private:
  std::string policy_;
  WeightsCfg weights_;
  std::mutex mu_;
  size_t rr_index_ = 0;
  std::mt19937 rng_;
};

}  // namespace pg
