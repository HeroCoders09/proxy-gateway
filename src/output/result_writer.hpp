#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <stdexcept>
#include <mutex>
#include <string>
#include <thread>

#include "common/config.hpp"
#include "common/metrics.hpp"

namespace pg {

struct RequestResult {
  int64_t timestamp_unix_ms = 0;
  std::string url;
  int status = 0;
  int proxy_id = -1;
  double latency_ms = 0.0;
  bool success = false;
};

// Nonblocking producer interface; disk writes happen on a dedicated thread.
class ResultWriter {
 public:
  ResultWriter(const OutputCfg& cfg, MetricsSink& metrics);
  ~ResultWriter();
  ResultWriter(const ResultWriter&) = delete;
  ResultWriter& operator=(const ResultWriter&) = delete;

  void start();
  void stop();
  bool record(RequestResult row);
  std::vector<RequestResult> recent(size_t limit = 50) const;

 private:
  void loop();
  std::string serialize_csv(const RequestResult& row) const;
  std::string serialize_json(const RequestResult& row) const;

  const OutputCfg cfg_;
  MetricsSink& metrics_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<RequestResult> queue_;
  std::deque<RequestResult> recent_;
  std::thread thread_;
  bool stopping_ = false;
  std::ofstream file_;
  static constexpr size_t kQueueLimit = 4096;
};

// Redacts authority credentials, path, and query data before any URL is persisted.
std::string safe_result_url(const std::string& url);

}  // namespace pg
