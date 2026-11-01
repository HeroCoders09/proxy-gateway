#pragma once
// Small HTTP/1.1 helpers shared by the request parser, the response parser and the forwarder.
#include <string>
#include <string_view>

#include "common/types.hpp"

namespace pg {

std::string to_lower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
std::string_view trim_ows(std::string_view s);

// Is `c` a valid RFC 9110 "tchar" (token character)?
bool is_tchar(char c);
bool is_token(std::string_view s);

// First header with this name (case-insensitive), or nullptr.
const std::string* find_header(const Headers& headers, std::string_view name);

// Parse "Name: value\r\n" lines. `block` holds the lines WITHOUT the final blank line.
// Rejects obs-fold, missing colon, bad names, control characters and more than `max_headers`.
// Returns false and sets `err` (a fixed string, never echoing raw input) on failure.
bool parse_header_lines(std::string_view block, Headers& out, std::string& err, size_t max_headers = 100);

// Strict decimal parse (digits only, no sign, no overflow past `max`).
bool parse_uint(std::string_view s, unsigned long long max, unsigned long long& out);

// Remove hop-by-hop headers (RFC 9110 section 7.6.1 plus Proxy-Connection).
void strip_hop_by_hop(Headers& headers);

}  // namespace pg
