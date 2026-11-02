#include "output/result_writer.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

namespace pg {

ResultWriter::ResultWriter(const OutputCfg& cfg, MetricsSink& metrics) : cfg_(cfg), metrics_(metrics) {}
ResultWriter::~ResultWriter() { stop(); }

void ResultWriter::start() {
  if (cfg_.format == "none" || thread_.joinable()) return;
  file_.open(cfg_.path, std::ios::out | std::ios::app);
  if (!file_) throw std::runtime_error("cannot open result output: " + cfg_.path);
  if (cfg_.format == "csv" && file_.tellp() == 0)
    file_ << "timestamp_unix_ms,url,status,proxy,latency_ms,success\n";
  stopping_ = false;
  thread_ = std::thread([this] { loop(); });
}

void ResultWriter::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (file_.is_open()) file_.close();
}

bool ResultWriter::record(RequestResult row) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    recent_.push_front(row);
    if (recent_.size() > 50) recent_.pop_back();
    if (cfg_.format == "none") return true;
    if (stopping_ || queue_.size() >= kQueueLimit) {
      metrics_.inc(Counter::OutputRowsDropped);
      return false;
    }
    queue_.push_back(std::move(row));
  }
  cv_.notify_one();
  return true;
}

std::vector<RequestResult> ResultWriter::recent(size_t limit) const {
  std::lock_guard<std::mutex> lock(mu_);
  const size_t count = std::min(limit, recent_.size());
  return {recent_.begin(), recent_.begin() + static_cast<std::ptrdiff_t>(count)};
}

void ResultWriter::loop() {
  for (;;) {
    RequestResult row;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty() && stopping_) break;
      row = std::move(queue_.front());
      queue_.pop_front();
    }
    file_ << (cfg_.format == "csv" ? serialize_csv(row) : serialize_json(row)) << '\n';
    if (!file_) metrics_.inc(Counter::OutputRowsDropped);
  }
  file_.flush();
}

std::string ResultWriter::serialize_csv(const RequestResult& row) const {
  std::ostringstream out;
  out << row.timestamp_unix_ms << ",\"";
  for (char c : row.url) { if (c == '"') out << '"'; out << c; }
  out << "\"," << row.status << ',' << row.proxy_id << ',' << std::fixed << std::setprecision(3)
      << row.latency_ms << ',' << (row.success ? "true" : "false");
  return out.str();
}

std::string ResultWriter::serialize_json(const RequestResult& row) const {
  return nlohmann::json{{"timestamp_unix_ms", row.timestamp_unix_ms}, {"url", row.url},
                        {"status", row.status}, {"proxy", row.proxy_id},
                        {"latency_ms", row.latency_ms}, {"success", row.success}}.dump();
}

std::string safe_result_url(const std::string& url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) return "[redacted]";
  const size_t authority_start = scheme_end + 3;
  const size_t authority_end = url.find_first_of("/?#", authority_start);
  const std::string authority = url.substr(authority_start,
      authority_end == std::string::npos ? std::string::npos : authority_end - authority_start);
  const auto at = authority.rfind('@');
  const std::string host = at == std::string::npos ? authority : authority.substr(at + 1);
  return url.substr(0, authority_start) + host + "/[redacted]";
}

}  // namespace pg
