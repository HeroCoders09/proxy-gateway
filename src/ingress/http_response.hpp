#pragma once
// Client-facing HTTP response: building, serialising and writing it to a socket.
// Later phases (workers writing replies asynchronously) reuse write_response().
#include <string>
#include <string_view>

#include "common/types.hpp"

namespace pg {

struct HttpResponse {
  int status = 200;
  Headers headers;
  std::string body;
};

const char* reason_phrase(int status);

// Gateway-generated error (400, 429, 502, 503, 504 ...). Body is a short fixed text/plain message.
HttpResponse make_error_response(int status, std::string_view detail);

// Always adds Content-Length (computed from body) and "Connection: close".
// Drops any Content-Length / Connection / Transfer-Encoding already present in `r.headers`.
// For HEAD replies pass head_only=true: the body is omitted and an upstream Content-Length is kept.
std::string serialize_response(const HttpResponse& r, bool head_only = false);

// Write all bytes, waiting for writability up to timeout_ms in total. Uses MSG_NOSIGNAL.
// Returns false on error, reset or timeout (never raises SIGPIPE).
bool write_all(int fd, std::string_view data, int timeout_ms);

// serialize + write_all.
bool write_response(int fd, const HttpResponse& r, bool head_only, int timeout_ms);

}  // namespace pg
