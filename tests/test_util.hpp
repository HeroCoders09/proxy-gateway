#pragma once
// Test helpers: raw TCP client, and an in-process fake server (acts as a fake proxy / target).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <dirent.h>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/fd.hpp"

namespace pgtest {

inline int count_open_fds() {
  int n = 0;
  if (DIR* d = ::opendir("/proc/self/fd")) {
    while (::readdir(d) != nullptr) ++n;
    ::closedir(d);
  }
  return n;
}

inline pg::Fd connect_local(uint16_t port) {
  pg::Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return pg::Fd();
  return fd;
}

// Send `request`, read until the server closes (or timeout), return everything received.
inline std::string roundtrip(uint16_t port, const std::string& request, int timeout_ms = 3000) {
  pg::Fd fd = connect_local(port);
  if (!fd) return "<connect failed>";
  if (!request.empty()) ::send(fd.get(), request.data(), request.size(), MSG_NOSIGNAL);
  std::string out;
  char buf[4096];
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) break;
    pollfd p{fd.get(), POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(left)) <= 0) break;
    ssize_t n = ::recv(fd.get(), buf, sizeof buf, 0);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  return out;
}

inline int status_of(const std::string& response) {
  if (response.size() < 12 || response.compare(0, 5, "HTTP/") != 0) return -1;
  return std::atoi(response.c_str() + 9);
}

// Fake server on 127.0.0.1:<ephemeral>. For each connection it reads one request head
// (up to the blank line), records it, then calls `reply(request_head)`. An empty reply means
// "send nothing and keep the connection open" (to exercise timeouts); `close_after` controls
// whether the server closes right after replying.
class FakeServer {
 public:
  using Reply = std::function<std::string(const std::string&)>;

  explicit FakeServer(Reply reply) : reply_(std::move(reply)) {
    listen_.reset(::socket(AF_INET, SOCK_STREAM, 0));
    int one = 1;
    ::setsockopt(listen_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ::bind(listen_.get(), reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::listen(listen_.get(), 64);
    socklen_t l = sizeof a;
    ::getsockname(listen_.get(), reinterpret_cast<sockaddr*>(&a), &l);
    port_ = ntohs(a.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~FakeServer() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    for (auto& t : conn_threads_) t.join();
  }

  uint16_t port() const { return port_; }
  std::vector<std::string> requests() {
    std::lock_guard<std::mutex> lk(mu_);
    return seen_;
  }

 private:
  void run() {
    while (!stop_) {
      pollfd p{listen_.get(), POLLIN, 0};
      if (::poll(&p, 1, 50) <= 0) continue;
      int c = ::accept(listen_.get(), nullptr, nullptr);
      if (c < 0) continue;
      conn_threads_.emplace_back([this, c] { serve(pg::Fd(c)); });
    }
  }
  void serve(pg::Fd c) {
    std::string head;
    char buf[2048];
    while (head.find("\r\n\r\n") == std::string::npos && !stop_) {
      pollfd p{c.get(), POLLIN, 0};
      if (::poll(&p, 1, 50) <= 0) continue;
      ssize_t n = ::recv(c.get(), buf, sizeof buf, 0);
      if (n <= 0) return;
      head.append(buf, static_cast<size_t>(n));
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      seen_.push_back(head);
    }
    std::string r = reply_(head);
    if (r.empty()) {  // hang until the test ends
      while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(20));
      return;
    }
    ::send(c.get(), r.data(), r.size(), MSG_NOSIGNAL);
  }

  Reply reply_;
  pg::Fd listen_;
  uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::vector<std::thread> conn_threads_;
  std::mutex mu_;
  std::vector<std::string> seen_;
};

}  // namespace pgtest
