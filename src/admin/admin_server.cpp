#include "admin/admin_server.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>
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

const char* kUiPlaceholder =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>Proxy Gateway</title></head>"
    "<body><h1>Intelligent Concurrent Proxy Gateway</h1>"
    "<p>Dashboard placeholder (Phase 1). The real dashboard is built in Phase 6.</p></body></html>";

}  // namespace

AdminServer::AdminServer(const GatewayConfig& cfg, Metrics& metrics, std::function<Phase2Stats()> phase2,
                         std::function<std::vector<ProxyState>()> proxy_snapshot)
    : cfg_(cfg), metrics_(metrics), phase2_(std::move(phase2)), proxy_snapshot_(std::move(proxy_snapshot)) {}

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
    return json_reply({{"status", "ok"}, {"uptime_seconds", uptime}, {"version", "0.1.0"}, {"phase", 3}, {"stub", true}});
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
                     {"success_rate", attempts ? static_cast<double>(successes) / attempts : 1.0},
                     {"active_connections", live ? state->active_connections : 0},
                     {"circuit_state", live ? to_string(state->circuit_state) : "CLOSED"},
                     {"circuit_open_count", 0},
                     {"last_probe_unix_ms", 0}});
    }
    return json_reply({{"stub", true}, {"proxies", arr}});
  }

  if (path == "/api/stats") {
    LatencyPercentiles lp = metrics_.latency_percentiles();
    uint64_t ok = metrics_.get(Counter::RequestsSuccessful);
    uint64_t failed = metrics_.get(Counter::RequestsFailed);
    double success_rate = (ok + failed) == 0 ? 1.0 : static_cast<double>(ok) / static_cast<double>(ok + failed);
    Phase2Stats p2 = phase2_ ? phase2_() : Phase2Stats{};
    json queue = p2.queue_length;
    return json_reply({{"stub", true},
                       {"timestamp_unix_ms", unix_ms_now()},
                       {"policy", cfg_.policy},
                       {"requests_total", metrics_.get(Counter::RequestsTotal)},
                       {"requests_successful", ok},
                       {"requests_failed", failed},
                       {"timeouts", metrics_.get(Counter::Timeouts)},
                       {"retries", metrics_.get(Counter::Retries)},
                       {"no_route", metrics_.get(Counter::NoRoute)},
                       {"selections", {{"random", metrics_.get(Counter::RandomSelections)},
                                       {"roundrobin", metrics_.get(Counter::RoundRobinSelections)},
                                       {"leastconn", metrics_.get(Counter::LeastConnSelections)},
                                       {"latency", metrics_.get(Counter::LatencySelections)},
                                       {"weighted", metrics_.get(Counter::WeightedSelections)}}},
                       {"requests_per_second", 0.0},
                       {"success_rate", success_rate},
                       {"active_connections", p2.active_jobs},
                       {"worker_count", p2.worker_count},
                       {"aimd_limit", p2.aimd_limit},
                       {"queue_length", queue},
                       {"latency_ms", {{"p50", lp.p50}, {"p95", lp.p95}, {"p99", lp.p99}}}});
  }

  if (path == "/metrics") {
    LatencyPercentiles lp = metrics_.latency_percentiles();
    std::ostringstream o;
    o << "# TYPE requests_total counter\nrequests_total " << metrics_.get(Counter::RequestsTotal) << "\n"
      << "# TYPE requests_successful counter\nrequests_successful " << metrics_.get(Counter::RequestsSuccessful) << "\n"
      << "# TYPE requests_failed counter\nrequests_failed " << metrics_.get(Counter::RequestsFailed) << "\n"
      << "# TYPE timeouts counter\ntimeouts " << metrics_.get(Counter::Timeouts) << "\n"
      << "# TYPE bad_requests counter\nbad_requests " << metrics_.get(Counter::BadRequests) << "\n"
      << "# TYPE queue_rejected counter\nqueue_rejected " << metrics_.get(Counter::QueueRejected) << "\n";
    o << "# TYPE retries counter\nretries " << metrics_.get(Counter::Retries) << "\n"
      << "# TYPE no_route counter\nno_route " << metrics_.get(Counter::NoRoute) << "\n"
      << "# TYPE routing_selections counter\n";
    for (const auto& [name, counter] : std::vector<std::pair<const char*, Counter>>{
             {"random", Counter::RandomSelections}, {"roundrobin", Counter::RoundRobinSelections},
             {"leastconn", Counter::LeastConnSelections}, {"latency", Counter::LatencySelections},
             {"weighted", Counter::WeightedSelections}})
      o << "routing_selections{policy=\"" << name << "\"} " << metrics_.get(counter) << "\n";
    Phase2Stats p2 = phase2_ ? phase2_() : Phase2Stats{};
    o << "# TYPE active_connections gauge\nactive_connections " << p2.active_jobs << "\n"
      << "# TYPE requests_per_second gauge\nrequests_per_second 0\n"
      << "# TYPE worker_count gauge\nworker_count " << p2.worker_count << "\n"
      << "# TYPE aimd_limit gauge\naimd_limit " << p2.aimd_limit << "\n"
      << "# TYPE queue_length gauge\n";
    for (const auto& [domain, size] : p2.queue_length) o << "queue_length{domain=\"" << domain << "\"} " << size << "\n";
    o
      << "# TYPE latency_ms summary\n"
      << "latency_ms{quantile=\"0.5\"} " << lp.p50 << "\n"
      << "latency_ms{quantile=\"0.95\"} " << lp.p95 << "\n"
      << "latency_ms{quantile=\"0.99\"} " << lp.p99 << "\n"
      << "# TYPE proxy_rtt gauge\n# TYPE proxy_success_rate gauge\n# TYPE proxy_failure_count counter\n";
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
        << (attempts ? static_cast<double>(successes) / attempts : 1.0) << "\n"
        << "proxy_failure_count{proxy=\"" << p.id << "\"} " << failures << "\n";
    }
    r.status = 200;
    r.content_type = "text/plain; version=0.0.4";
    r.body = o.str();
    return r;
  }

  if (path == "/ui" || path == "/ui/") {
    r.status = 200;
    r.content_type = "text/html; charset=utf-8";
    r.body = kUiPlaceholder;
    return r;
  }

  r.status = 404;
  r.content_type = "application/json";
  r.body = json{{"error", "not found"}}.dump();
  return r;
}

}  // namespace pg
