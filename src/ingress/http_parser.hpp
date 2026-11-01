#pragma once
// Parser for proxy-style HTTP/1.1 requests with absolute URLs ("GET http://host/path HTTP/1.1").
// Incremental: call with the bytes received so far; it says Complete / NeedMore / Malformed.
// Never throws and never reads outside `buf`, so garbage input cannot crash the gateway.
#include <cstdint>
#include <string>
#include <string_view>

#include "common/types.hpp"

namespace pg {

enum class ParseStatus { Complete, NeedMore, Malformed, TooLarge };

struct ParseLimits {
  size_t max_header_bytes = 16384;
  size_t max_body_bytes = 1048576;
};

struct ParsedRequest {
  std::string method;
  std::string url;           // absolute URL exactly as received
  std::string host;          // lower-case, no port
  uint16_t port = 80;
  std::string path;          // path + query, starts with '/'
  Headers headers;
  std::string body;
};

struct ParseResult {
  ParseStatus status = ParseStatus::NeedMore;
  std::string reason;        // fixed, safe-to-return text for Malformed / TooLarge
  size_t consumed = 0;       // bytes used by the request when Complete
  ParsedRequest request;
};

ParseResult parse_request(std::string_view buf, const ParseLimits& limits);

// Split "http://host[:port][/path?query]" into parts. Returns false and sets `err` on failure.
// Only http:// is accepted in Phase 1 (see docs/ISSUES.md I-001).
bool parse_absolute_url(std::string_view url, std::string& host, uint16_t& port, std::string& path,
                        std::string& err);

}  // namespace pg
