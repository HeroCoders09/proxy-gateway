#include "ingress/http_response.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>

#include "common/http_util.hpp"

namespace pg {

const char* reason_phrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 407: return "Proxy Authentication Required";
    case 413: return "Content Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Status";
  }
}

HttpResponse make_error_response(int status, std::string_view detail) {
  HttpResponse r;
  r.status = status;
  r.headers.push_back({"Content-Type", "text/plain; charset=utf-8"});
  r.body = "gateway error " + std::to_string(status) + " " + reason_phrase(status);
  if (!detail.empty()) {
    r.body += ": ";
    r.body += std::string(detail);
  }
  r.body += "\n";
  return r;
}

std::string serialize_response(const HttpResponse& r, bool head_only) {
  std::string out;
  out.reserve(256 + r.body.size());
  out += "HTTP/1.1 ";
  out += std::to_string(r.status);
  out += ' ';
  out += reason_phrase(r.status);
  out += "\r\n";

  std::string upstream_len;
  for (const auto& h : r.headers) {
    if (iequals(h.name, "content-length")) {
      upstream_len = h.value;
      continue;
    }
    if (iequals(h.name, "connection") || iequals(h.name, "transfer-encoding")) continue;
    // Defence in depth: refuse to emit header injection even if an upstream value was odd.
    if (h.name.find_first_of("\r\n") != std::string::npos || h.value.find_first_of("\r\n") != std::string::npos) {
      continue;
    }
    out += h.name;
    out += ": ";
    out += h.value;
    out += "\r\n";
  }
  out += "Content-Length: ";
  if (head_only && !upstream_len.empty()) {
    out += upstream_len;
  } else {
    out += std::to_string(r.body.size());
  }
  out += "\r\nConnection: close\r\n\r\n";
  if (!head_only) out += r.body;
  return out;
}

bool write_all(int fd, std::string_view data, int timeout_ms) {
  using clock = std::chrono::steady_clock;
  auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
      if (left <= 0) return false;
      pollfd p{fd, POLLOUT, 0};
      int rc = ::poll(&p, 1, static_cast<int>(left));
      if (rc < 0 && errno == EINTR) continue;
      if (rc <= 0) return false;
      continue;
    }
    return false;  // EPIPE, ECONNRESET, ...
  }
  return true;
}

bool write_response(int fd, const HttpResponse& r, bool head_only, int timeout_ms) {
  return write_all(fd, serialize_response(r, head_only), timeout_ms);
}

}  // namespace pg
