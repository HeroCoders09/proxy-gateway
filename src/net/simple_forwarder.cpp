#include "net/simple_forwarder.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include "common/fd.hpp"
#include "common/http_util.hpp"
#include "ingress/http_response.hpp"
#include "net/http_response_parser.hpp"

namespace pg {

namespace {

using clock_t_ = std::chrono::steady_clock;

long long ms_left(clock_t_::time_point deadline) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_t_::now()).count();
}

struct ConnectOutcome {
  Fd fd;
  ErrorKind error = ErrorKind::None;
  std::string detail;
};

ConnectOutcome connect_with_timeout(const std::string& host, uint16_t port, int timeout_ms) {
  ConnectOutcome out;
  const auto deadline = clock_t_::now() + std::chrono::milliseconds(timeout_ms);

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  int gai = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
  if (gai != 0 || res == nullptr) {
    out.error = ErrorKind::ConnectFailed;
    out.detail = "cannot resolve proxy host";
    return out;
  }

  ErrorKind last_err = ErrorKind::ConnectFailed;
  std::string last_detail = "connect failed";
  for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    long long left = ms_left(deadline);
    if (left <= 0) {
      last_err = ErrorKind::ConnectTimeout;
      last_detail = "connect timed out";
      break;
    }
    Fd fd(::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol));
    if (!fd) continue;
    int rc = ::connect(fd.get(), ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
      last_detail = std::string("connect failed: ") + std::strerror(errno);
      continue;
    }
    if (rc != 0) {
      pollfd p{fd.get(), POLLOUT, 0};
      int pr;
      do {
        left = ms_left(deadline);
        if (left < 0) left = 0;
        pr = ::poll(&p, 1, static_cast<int>(left));
      } while (pr < 0 && errno == EINTR);
      if (pr == 0) {
        last_err = ErrorKind::ConnectTimeout;
        last_detail = "connect timed out";
        break;
      }
      if (pr < 0) {
        last_detail = "poll failed during connect";
        continue;
      }
      int soerr = 0;
      socklen_t sl = sizeof soerr;
      ::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &soerr, &sl);
      if (soerr != 0) {
        last_detail = std::string("connect failed: ") + std::strerror(soerr);
        continue;
      }
    }
    int one = 1;
    ::setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    out.fd = std::move(fd);
    out.error = ErrorKind::None;
    ::freeaddrinfo(res);
    return out;
  }
  ::freeaddrinfo(res);
  out.error = last_err;
  out.detail = last_detail;
  return out;
}

}  // namespace

std::string build_proxy_request(const Job& job) {
  const bool default_port = (job.target_port == 80);
  std::string authority = job.target_host;
  if (!default_port) authority += ":" + std::to_string(job.target_port);

  std::string req;
  req.reserve(256 + job.body.size());
  req += job.method;
  req += " http://";
  req += authority;
  req += job.target_path;
  req += " HTTP/1.1\r\nHost: ";
  req += authority;
  req += "\r\n";

  Headers forward = job.headers;
  strip_hop_by_hop(forward);  // drops Connection, Keep-Alive, Proxy-*, TE, Transfer-Encoding, Upgrade ...
  for (const auto& h : forward) {
    if (iequals(h.name, "host") || iequals(h.name, "content-length")) continue;
    if (h.name.find_first_of("\r\n") != std::string::npos || h.value.find_first_of("\r\n") != std::string::npos) continue;
    req += h.name;
    req += ": ";
    req += h.value;
    req += "\r\n";
  }
  const bool has_body_method = (job.method == "POST" || job.method == "PUT" || job.method == "PATCH");
  if (!job.body.empty() || has_body_method) {
    req += "Content-Length: " + std::to_string(job.body.size()) + "\r\n";
  }
  req += "Connection: close\r\n\r\n";
  req += job.body;
  return req;
}

SimpleForwarder::SimpleForwarder(int connect_timeout_ms, int response_timeout_ms, size_t max_response_bytes)
    : connect_timeout_ms_(connect_timeout_ms),
      response_timeout_ms_(response_timeout_ms),
      max_response_bytes_(max_response_bytes) {}

ForwardResult SimpleForwarder::forward(const Job& job, const ProxyState& proxy) {
  const auto t0 = clock_t_::now();
  ForwardResult result;
  result.proxy_id = proxy.id;

  auto finish = [&](ErrorKind kind, std::string detail) {
    result.error = kind;
    result.error_detail = std::move(detail);
    result.latency_ms = std::chrono::duration<double, std::milli>(clock_t_::now() - t0).count();
    return result;
  };

  ConnectOutcome conn = connect_with_timeout(proxy.host, proxy.port, connect_timeout_ms_);
  if (conn.error != ErrorKind::None) return finish(conn.error, conn.detail);

  // The response clock starts once the connection is established.
  const auto deadline = clock_t_::now() + std::chrono::milliseconds(response_timeout_ms_);

  const std::string request = build_proxy_request(job);
  long long left = ms_left(deadline);
  if (left <= 0 || !write_all(conn.fd.get(), request, static_cast<int>(left))) {
    return finish(left <= 0 ? ErrorKind::ResponseTimeout : ErrorKind::IoError, "failed to send request to proxy");
  }

  const bool head_request = (job.method == "HEAD");
  std::string buf;
  char chunk[16384];
  bool eof = false;

  for (;;) {
    RespParseResult pr = parse_response(buf, head_request, eof, max_response_bytes_);
    if (pr.status == RespStatus::Complete) {
      result.status = pr.response.status;
      result.headers = std::move(pr.response.headers);
      result.body = std::move(pr.response.body);
      return finish(ErrorKind::None, "");
    }
    if (pr.status == RespStatus::TooLarge) return finish(ErrorKind::BadResponse, pr.error);
    if (pr.status == RespStatus::Malformed) {
      if (eof && buf.empty()) return finish(ErrorKind::IoError, "proxy closed the connection without a response");
      return finish(ErrorKind::BadResponse, pr.error);
    }
    // NeedMore
    if (eof) return finish(ErrorKind::BadResponse, "connection closed before response completed");

    left = ms_left(deadline);
    if (left <= 0) return finish(ErrorKind::ResponseTimeout, "response timed out");
    pollfd p{conn.fd.get(), POLLIN, 0};
    int rc = ::poll(&p, 1, static_cast<int>(left));
    if (rc < 0) {
      if (errno == EINTR) continue;
      return finish(ErrorKind::IoError, "poll failed while reading response");
    }
    if (rc == 0) return finish(ErrorKind::ResponseTimeout, "response timed out");

    ssize_t n = ::recv(conn.fd.get(), chunk, sizeof chunk, 0);
    if (n > 0) {
      buf.append(chunk, static_cast<size_t>(n));
      if (buf.size() > max_response_bytes_) return finish(ErrorKind::BadResponse, "response too large");
    } else if (n == 0) {
      eof = true;
    } else if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      continue;
    } else {
      return finish(ErrorKind::IoError, std::string("read from proxy failed: ") + std::strerror(errno));
    }
  }
}

}  // namespace pg
