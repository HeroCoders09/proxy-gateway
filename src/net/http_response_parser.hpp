#pragma once
// Parser for HTTP/1.1 responses coming back from a proxy. Handles Content-Length, chunked
// transfer coding and read-until-close framing. Pure function over a byte buffer so it can be unit
// tested and reused by the Phase 5 epoll forwarder.
#include <string>
#include <string_view>

#include "common/types.hpp"

namespace pg {

enum class RespStatus { Complete, NeedMore, Malformed, TooLarge };

struct ParsedResponse {
  int status = 0;
  std::string reason;
  Headers headers;   // as sent by the upstream (still includes Content-Length / Transfer-Encoding)
  std::string body;  // de-chunked
  bool upstream_wants_close = true;
};

struct RespParseResult {
  RespStatus status = RespStatus::NeedMore;
  std::string error;  // fixed text for Malformed / TooLarge
  ParsedResponse response;
};

// Parse everything received so far.
//   head_request : the request was HEAD (no body regardless of headers)
//   eof          : the peer has closed; needed to finish read-until-close bodies
//   max_total    : hard cap on buffered bytes (TooLarge when exceeded)
// Interim 1xx responses (except 101) are skipped.
RespParseResult parse_response(std::string_view buf, bool head_request, bool eof, size_t max_total);

}  // namespace pg
