#include "admin/admin_server.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "common/log.hpp"
#include "ingress/http_response.hpp"

namespace pg {

using json = nlohmann::json;

namespace {

long long unix_ms_now() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

AdminServer::AdminServer(const GatewayConfig& cfg, Metrics& metrics, std::function<Phase2Stats()> phase2,
                         std::function<std::vector<ProxyState>()> proxy_snapshot,
                         std::function<std::vector<RequestResult>()> recent_snapshot,
                         std::function<std::string()> policy_snapshot,
                         std::function<bool(const std::string&)> policy_setter,
                         std::function<void(double,double)> rate_setter,
                         std::function<bool()> health_aware_snapshot,
                         std::function<void(bool)> health_aware_setter)
    : cfg_(cfg), metrics_(metrics), phase2_(std::move(phase2)), proxy_snapshot_(std::move(proxy_snapshot)),
      recent_snapshot_(std::move(recent_snapshot)), policy_snapshot_(std::move(policy_snapshot)),
      policy_setter_(std::move(policy_setter)), rate_setter_(std::move(rate_setter)),
      health_aware_snapshot_(std::move(health_aware_snapshot)), health_aware_setter_(std::move(health_aware_setter)) {}

AdminServer::~AdminServer() { stop(); }

void AdminServer::start() {
  Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd) throw std::runtime_error(std::string("admin socket() failed: ") + std::strerror(errno));
  int one = 1;
  ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg_.admin_port);
  if (::inet_pton(AF_INET, cfg_.admin_host.c_str(), &addr.sin_addr) != 1) {
    throw std::runtime_error("admin.host must be an IPv4 address, got '" + cfg_.admin_host + "'");
  }
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    throw std::runtime_error("cannot bind admin " + cfg_.admin_host + ":" + std::to_string(cfg_.admin_port) +
                             ": " + std::strerror(errno));
  }
  if (::listen(fd.get(), 64) != 0) throw std::runtime_error(std::string("admin listen() failed: ") + std::strerror(errno));
  socklen_t len = sizeof addr;
  ::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &len);
  bound_port_ = ntohs(addr.sin_port);
  listen_fd_ = std::move(fd);
  stop_ = false;
  thread_ = std::thread([this] { loop(); });
}

void AdminServer::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  listen_fd_.reset();
}

void AdminServer::loop() {
  while (!stop_) {
    pollfd p{listen_fd_.get(), POLLIN, 0};
    int rc = ::poll(&p, 1, 200);
    if (rc < 0 && errno != EINTR) break;
    if (rc <= 0) continue;
    Fd client(::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC));
    if (!client) continue;
    try {
      serve(std::move(client));
    } catch (const std::exception& e) {
      log_error(std::string("admin error: ") + e.what());
    }
  }
}

void AdminServer::serve(Fd client) {
  // One short request at a time (it is a stub); 2 s to deliver the request head.
  std::string buf;
  char chunk[2048];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (buf.find("\r\n\r\n") == std::string::npos && buf.size() < 8192) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) return;
    pollfd p{client.get(), POLLIN, 0};
    int rc = ::poll(&p, 1, static_cast<int>(left));
    if (rc < 0 && errno == EINTR) continue;
    if (rc <= 0) return;
    ssize_t n = ::recv(client.get(), chunk, sizeof chunk, 0);
    if (n <= 0) return;
    buf.append(chunk, static_cast<size_t>(n));
  }
  size_t eol = buf.find("\r\n");
  std::string line = buf.substr(0, eol == std::string::npos ? buf.size() : eol);
  size_t s1 = line.find(' ');
  size_t s2 = (s1 == std::string::npos) ? std::string::npos : line.find(' ', s1 + 1);
  HttpResponse resp;
  if (s1 == std::string::npos || s2 == std::string::npos) {
    resp = make_error_response(400, "malformed request line");
  } else {
    Reply r = route(line.substr(0, s1), line.substr(s1 + 1, s2 - s1 - 1));
    resp.status = r.status;
    resp.headers.push_back({"Content-Type", r.content_type});
    resp.body = std::move(r.body);
  }
  write_response(client.get(), resp, false, 2000);
}

AdminServer::Reply AdminServer::route(const std::string& method, const std::string& target) const {
  Reply r;
  auto json_reply = [&](const json& j) {
    r.status = 200;
    r.content_type = "application/json";
    r.body = j.dump();
    return r;
  };

  if (method != "GET") {
    r.status = 405;
    r.content_type = "application/json";
    r.body = json{{"error", "method not allowed"}}.dump();
    return r;
  }
  std::string path = target.substr(0, target.find('?'));

  if (path == "/health") {
    double uptime = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    return json_reply({{"status", "ok"}, {"uptime_seconds", uptime}, {"version", "0.1.0"}, {"phase", 6}, {"stub", false}});
  }

  if (path == "/proxies") {
    json arr = json::array();
    const std::vector<ProxyState> states = proxy_snapshot_ ? proxy_snapshot_() : std::vector<ProxyState>{};
    for (const auto& p : cfg_.proxies) {
      auto state = std::find_if(states.begin(), states.end(), [&](const auto& s) { return s.id == p.id; });
      const bool live = state != states.end();
      const uint64_t successes = live ? state->success_count : 0;
      const uint64_t failures = live ? state->failure_count : 0;
      const uint64_t attempts = successes + failures;
      arr.push_back({{"id", p.id},
                     {"host", p.host},
                     {"port", p.port},
                     {"healthy", live ? state->healthy : true},
                     {"rtt_ms", live ? state->rtt_ms : 0.0},
                     {"success_count", successes},
                     {"failure_count", failures},
                     {"requests_sent", attempts},
                     {"consecutive_failures", live ? state->consecutive_failures : 0},
                     {"success_rate", live ? state->rolling_success_rate : (attempts ? static_cast<double>(successes) / attempts : 1.0)},
                     {"active_connections", live ? state->active_connections : 0},
                     {"circuit_state", live ? to_string(state->circuit_state) : "CLOSED"},
                     {"circuit_open_count", live ? state->circuit_open_count : 0},
                     {"last_probe_unix_ms", live ? state->last_probe_unix_ms : 0},
                     {"rolling_rtt_ms", live ? state->rolling_rtt_ms : 0.0},
                     {"probe_success_count", live ? state->probe_success_count : 0},
                     {"probe_failure_count", live ? state->probe_failure_count : 0}});
    }
    return json_reply({{"stub", false}, {"proxies", arr}});
  }

  if (path == "/api/stats") {
    LatencyPercentiles lp = metrics_.latency_percentiles();
    LatencyPercentiles overhead = metrics_.gateway_overhead_percentiles();
    uint64_t ok = metrics_.get(Counter::RequestsSuccessful);
    uint64_t failed = metrics_.get(Counter::RequestsFailed);
    double success_rate = (ok + failed) == 0 ? 1.0 : static_cast<double>(ok) / static_cast<double>(ok + failed);
    Phase2Stats p2 = phase2_ ? phase2_() : Phase2Stats{};
    json queue = p2.queue_length;
    return json_reply({{"stub", false},
                       {"timestamp_unix_ms", unix_ms_now()},
                       {"policy", policy_snapshot_ ? policy_snapshot_() : cfg_.policy},
                       {"health_aware", health_aware_snapshot_ ? health_aware_snapshot_() : true},
                       {"requests_total", metrics_.get(Counter::RequestsTotal)},
                       {"requests_successful", ok},
                       {"requests_failed", failed},
                       {"output_rows_dropped", metrics_.get(Counter::OutputRowsDropped)},
                       {"timeouts", metrics_.get(Counter::Timeouts)},
                       {"upstream_connections_opened", metrics_.get(Counter::ConnectionsOpened)},
                       {"upstream_connections_reused", metrics_.get(Counter::ConnectionsReused)},
                       {"upstream_connect_timeouts", metrics_.get(Counter::ConnectTimeouts)},
                       {"upstream_response_timeouts", metrics_.get(Counter::ResponseTimeouts)},
                       {"upstream_bytes_in", metrics_.get(Counter::BytesIn)},
                       {"upstream_bytes_out", metrics_.get(Counter::BytesOut)},
                       {"upstream_active_connections", metrics_.get(Gauge::UpstreamActiveConnections)},
                       {"upstream_idle_connections", metrics_.get(Gauge::UpstreamIdleConnections)},
                       {"health_probe_successes", metrics_.get(Counter::ProbeSuccesses)},
                       {"health_probe_failures", metrics_.get(Counter::ProbeFailures)},
                       {"retries", metrics_.get(Counter::Retries)},
                       {"no_route", metrics_.get(Counter::NoRoute)},
                       {"selections", {{"random", metrics_.get(Counter::RandomSelections)},
                                       {"roundrobin", metrics_.get(Counter::RoundRobinSelections)},
                                       {"leastconn", metrics_.get(Counter::LeastConnSelections)},
                                       {"latency", metrics_.get(Counter::LatencySelections)},
                                       {"weighted", metrics_.get(Counter::WeightedSelections)}}},
                       {"requests_per_second", update_requests_per_second()},
                       {"success_rate", success_rate},
                       {"active_connections", p2.active_jobs},
                       {"worker_count", p2.worker_count},
                       {"aimd_limit", p2.aimd_limit},
                       {"queue_length", queue},
                       {"latency_ms", {{"p50", lp.p50}, {"p95", lp.p95}, {"p99", lp.p99}}},
                       {"gateway_overhead_ms", {{"p50", overhead.p50}, {"p95", overhead.p95},
                                                  {"p99", overhead.p99}}}});
  }

  if (path == "/api/recent") {
    json rows = json::array();
    if (recent_snapshot_) for (const auto& row : recent_snapshot_())
      rows.push_back({{"timestamp_unix_ms", row.timestamp_unix_ms}, {"url", row.url},
                      {"status", row.status}, {"proxy", row.proxy_id},
                      {"latency_ms", row.latency_ms}, {"success", row.success}});
    return json_reply({{"requests", rows}});
  }

  if (path == "/api/control/settings") {
    std::map<std::string, std::string> params;
    const size_t q = target.find('?');
    if (q != std::string::npos) {
      std::istringstream in(target.substr(q + 1));
      std::string item;
      while (std::getline(in, item, '&')) {
        const size_t eq = item.find('=');
        if (eq != std::string::npos) params[item.substr(0, eq)] = item.substr(eq + 1);
      }
    }
    double rate = 0.0, burst = 0.0;
    try {
      size_t rate_end = 0, burst_end = 0;
      rate = std::stod(params.at("rate"), &rate_end); burst = std::stod(params.at("burst"), &burst_end);
      if (rate_end != params.at("rate").size() || burst_end != params.at("burst").size()) throw std::invalid_argument("trailing characters");
    }
    catch (...) { r.status = 400; r.content_type = "application/json"; r.body = json{{"error", "rate and burst must be numbers"}}.dump(); return r; }
    const std::string policy = params.count("policy") ? params["policy"] : "";
    bool health_aware = health_aware_snapshot_ ? health_aware_snapshot_() : true;
    if (params.count("health_aware")) {
      if (params["health_aware"] != "true" && params["health_aware"] != "false") {
        r.status=400; r.content_type="application/json"; r.body=json{{"error","health_aware must be true or false"}}.dump(); return r;
      }
      health_aware = params["health_aware"] == "true";
    }
    if (!is_valid_policy(policy) || !std::isfinite(rate) || !std::isfinite(burst) ||
        rate < 1 || rate > 1000000 || burst < 1 || burst > 10000000) {
      r.status = 400; r.content_type = "application/json"; r.body = json{{"error", "invalid policy, rate, or burst"}}.dump(); return r;
    }
    if (!policy_setter_ || !policy_setter_(policy) || !rate_setter_) {
      r.status = 503; r.content_type = "application/json"; r.body = json{{"error", "control unavailable"}}.dump(); return r;
    }
    rate_setter_(rate, burst);
    if (health_aware_setter_) health_aware_setter_(health_aware);
    return json_reply({{"message", "Settings applied"}, {"policy", policy}, {"rate", rate}, {"burst", burst}, {"health_aware",health_aware}});
  }

  if (path == "/metrics") {
    LatencyPercentiles lp = metrics_.latency_percentiles();
    LatencyPercentiles overhead = metrics_.gateway_overhead_percentiles();
    const double rps = update_requests_per_second();
    std::ostringstream o;
    o << "# TYPE requests_total counter\nrequests_total " << metrics_.get(Counter::RequestsTotal) << "\n"
      << "# TYPE requests_successful counter\nrequests_successful " << metrics_.get(Counter::RequestsSuccessful) << "\n"
      << "# TYPE requests_failed counter\nrequests_failed " << metrics_.get(Counter::RequestsFailed) << "\n"
      << "# TYPE timeouts counter\ntimeouts " << metrics_.get(Counter::Timeouts) << "\n"
      << "# TYPE upstream_connections_opened_total counter\nupstream_connections_opened_total " << metrics_.get(Counter::ConnectionsOpened) << "\n"
      << "# TYPE upstream_connections_reused_total counter\nupstream_connections_reused_total " << metrics_.get(Counter::ConnectionsReused) << "\n"
      << "# TYPE upstream_connect_timeouts_total counter\nupstream_connect_timeouts_total " << metrics_.get(Counter::ConnectTimeouts) << "\n"
      << "# TYPE upstream_response_timeouts_total counter\nupstream_response_timeouts_total " << metrics_.get(Counter::ResponseTimeouts) << "\n"
      << "# TYPE upstream_bytes_in_total counter\nupstream_bytes_in_total " << metrics_.get(Counter::BytesIn) << "\n"
      << "# TYPE upstream_bytes_out_total counter\nupstream_bytes_out_total " << metrics_.get(Counter::BytesOut) << "\n"
      << "# TYPE bad_requests counter\nbad_requests " << metrics_.get(Counter::BadRequests) << "\n"
      << "# TYPE queue_rejected counter\nqueue_rejected " << metrics_.get(Counter::QueueRejected) << "\n";
    o << "# TYPE retries counter\nretries " << metrics_.get(Counter::Retries) << "\n"
      << "# TYPE output_rows_dropped_total counter\noutput_rows_dropped_total " << metrics_.get(Counter::OutputRowsDropped) << "\n"
      << "# TYPE no_route counter\nno_route " << metrics_.get(Counter::NoRoute) << "\n"
      << "# TYPE health_probe_successes_total counter\nhealth_probe_successes_total " << metrics_.get(Counter::ProbeSuccesses) << "\n"
      << "# TYPE health_probe_failures_total counter\nhealth_probe_failures_total " << metrics_.get(Counter::ProbeFailures) << "\n"
      << "# TYPE routing_selections counter\n";
    for (const auto& [name, counter] : std::vector<std::pair<const char*, Counter>>{
             {"random", Counter::RandomSelections}, {"roundrobin", Counter::RoundRobinSelections},
             {"leastconn", Counter::LeastConnSelections}, {"latency", Counter::LatencySelections},
             {"weighted", Counter::WeightedSelections}})
      o << "routing_selections{policy=\"" << name << "\"} " << metrics_.get(counter) << "\n";
    Phase2Stats p2 = phase2_ ? phase2_() : Phase2Stats{};
    o << "# TYPE active_connections gauge\nactive_connections " << p2.active_jobs << "\n"
      << "# TYPE upstream_active_connections gauge\nupstream_active_connections " << metrics_.get(Gauge::UpstreamActiveConnections) << "\n"
      << "# TYPE upstream_idle_connections gauge\nupstream_idle_connections " << metrics_.get(Gauge::UpstreamIdleConnections) << "\n"
      << "# TYPE requests_per_second gauge\nrequests_per_second " << rps << "\n"
      << "# TYPE worker_count gauge\nworker_count " << p2.worker_count << "\n"
      << "# TYPE aimd_limit gauge\naimd_limit " << p2.aimd_limit << "\n"
      << "# TYPE queue_length gauge\n";
    for (const auto& [domain, size] : p2.queue_length) o << "queue_length{domain=\"" << domain << "\"} " << size << "\n";
    o
      << "# TYPE latency_ms summary\n"
      << "latency_ms{quantile=\"0.5\"} " << lp.p50 << "\n"
      << "latency_ms{quantile=\"0.95\"} " << lp.p95 << "\n"
      << "latency_ms{quantile=\"0.99\"} " << lp.p99 << "\n"
      << "# TYPE gateway_overhead_ms summary\n"
      << "gateway_overhead_ms{quantile=\"0.5\"} " << overhead.p50 << "\n"
      << "gateway_overhead_ms{quantile=\"0.95\"} " << overhead.p95 << "\n"
      << "gateway_overhead_ms{quantile=\"0.99\"} " << overhead.p99 << "\n"
      << "# TYPE proxy_rtt gauge\n# TYPE proxy_rolling_rtt gauge\n# TYPE proxy_success_rate gauge\n# TYPE proxy_failure_count counter\n"
      << "# TYPE proxy_consecutive_failures gauge\n# TYPE proxy_circuit_open_count counter\n# TYPE proxy_last_probe_unix_ms gauge\n"
      << "# TYPE proxy_probe_successes_total counter\n# TYPE proxy_probe_failures_total counter\n";
    const std::vector<ProxyState> states = proxy_snapshot_ ? proxy_snapshot_() : std::vector<ProxyState>{};
    for (const auto& p : cfg_.proxies) {
      auto state = std::find_if(states.begin(), states.end(), [&](const auto& s) { return s.id == p.id; });
      const bool live = state != states.end();
      const uint64_t successes = live ? state->success_count : 0;
      const uint64_t failures = live ? state->failure_count : 0;
      const uint64_t attempts = successes + failures;
      o << "proxy_requests_total{proxy=\"" << p.id << "\"} " << attempts << "\n"
        << "proxy_successes_total{proxy=\"" << p.id << "\"} " << successes << "\n"
        << "proxy_failures_total{proxy=\"" << p.id << "\"} " << failures << "\n"
        << "proxy_active_connections{proxy=\"" << p.id << "\"} " << (live ? state->active_connections : 0) << "\n"
        << "proxy_rtt{proxy=\"" << p.id << "\"} " << (live ? state->rtt_ms : 0.0) << "\n"
        << "proxy_success_rate{proxy=\"" << p.id << "\"} "
        << (live ? state->rolling_success_rate : (attempts ? static_cast<double>(successes) / attempts : 1.0)) << "\n"
        << "proxy_failure_count{proxy=\"" << p.id << "\"} " << failures << "\n"
        << "proxy_rolling_rtt{proxy=\"" << p.id << "\"} " << (live ? state->rolling_rtt_ms : 0.0) << "\n"
        << "proxy_consecutive_failures{proxy=\"" << p.id << "\"} " << (live ? state->consecutive_failures : 0) << "\n"
        << "proxy_circuit_open_count{proxy=\"" << p.id << "\"} " << (live ? state->circuit_open_count : 0) << "\n"
        << "proxy_last_probe_unix_ms{proxy=\"" << p.id << "\"} " << (live ? state->last_probe_unix_ms : 0) << "\n"
        << "proxy_probe_successes_total{proxy=\"" << p.id << "\"} " << (live ? state->probe_success_count : 0) << "\n"
        << "proxy_probe_failures_total{proxy=\"" << p.id << "\"} " << (live ? state->probe_failure_count : 0) << "\n";
    }
    r.status = 200;
    r.content_type = "text/plain; version=0.0.4";
    r.body = o.str();
    return r;
  }

  if (path == "/ui" || path == "/ui/") {
    r.status = 200;
    r.content_type = "text/html; charset=utf-8";
    std::ifstream page("dashboard/index.html", std::ios::binary);
    if (!page) {
      r.status = 500;
      r.body = "Dashboard assets unavailable; run gateway from the project root.";
      return r;
    }
    std::ostringstream body;
    body << page.rdbuf();
    r.body = body.str();
    return r;
  }

  r.status = 404;
  r.content_type = "application/json";
  r.body = json{{"error", "not found"}}.dump();
  return r;
}

double AdminServer::update_requests_per_second() const {
  const auto now = std::chrono::steady_clock::now();
  const uint64_t total = metrics_.get(Counter::RequestsTotal);
  std::lock_guard<std::mutex> lock(rate_mu_);
  const double seconds = std::chrono::duration<double>(now - rate_last_sample_).count();
  if (seconds >= 0.25) {
    requests_per_second_ = static_cast<double>(total - rate_last_total_) / seconds;
    rate_last_total_ = total;
    rate_last_sample_ = now;
  }
  return requests_per_second_;
}

}  // namespace pg
