#include "ingress/http_parser.hpp"

#include "common/http_util.hpp"

namespace pg {

namespace {

ParseResult bad(const char* reason) {
  ParseResult r;
  r.status = ParseStatus::Malformed;
  r.reason = reason;
  return r;
}

ParseResult too_large(const char* reason) {
  ParseResult r;
  r.status = ParseStatus::TooLarge;
  r.reason = reason;
  return r;
}

bool method_allowed(std::string_view m) {
  return m == "GET" || m == "HEAD" || m == "POST" || m == "PUT" || m == "DELETE" || m == "OPTIONS" ||
         m == "PATCH";
}

}  // namespace

bool parse_absolute_url(std::string_view url, std::string& host, uint16_t& port, std::string& path,
                        std::string& err) {
  constexpr std::string_view kHttp = "http://";
  if (url.size() >= 8 && iequals(url.substr(0, 8), "https://")) {
    err = "https URLs are not supported; use an absolute http:// URL";
    return false;
  }
  if (url.size() < kHttp.size() || !iequals(url.substr(0, kHttp.size()), kHttp)) {
    err = "request target must be an absolute http:// URL";
    return false;
  }
  for (char c : url) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7f) {
      err = "control or space character in URL";
      return false;
    }
  }
  std::string_view rest = url.substr(kHttp.size());
  size_t auth_end = rest.find_first_of("/?#");
  std::string_view authority = rest.substr(0, auth_end);
  std::string_view tail = (auth_end == std::string_view::npos) ? std::string_view() : rest.substr(auth_end);

  if (authority.empty()) {
    err = "URL has no host";
    return false;
  }
  if (authority.find('@') != std::string_view::npos) {
    err = "userinfo in URL is not supported";
    return false;
  }
  if (authority.front() == '[') {
    err = "IPv6 literal hosts are not supported";
    return false;
  }

  std::string_view host_part = authority;
  unsigned long long p = 80;
  size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos) {
    host_part = authority.substr(0, colon);
    std::string_view port_part = authority.substr(colon + 1);
    if (!parse_uint(port_part, 65535, p) || p == 0) {
      err = "invalid port in URL";
      return false;
    }
  }
  if (host_part.empty() || host_part.size() > 253) {
    err = "invalid host in URL";
    return false;
  }
  for (char c : host_part) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '.' || c == '_';
    if (!ok) {
      err = "invalid character in host";
      return false;
    }
  }

  // Drop the fragment; normalise an empty path or a bare query to start with '/'.
  size_t hash = tail.find('#');
  if (hash != std::string_view::npos) tail = tail.substr(0, hash);
  std::string p_out;
  if (tail.empty()) {
    p_out = "/";
  } else if (tail.front() == '?') {
    p_out = "/" + std::string(tail);
  } else {
    p_out = std::string(tail);
  }

  host = to_lower(host_part);
  port = static_cast<uint16_t>(p);
  path = std::move(p_out);
  return true;
}

ParseResult parse_request(std::string_view buf, const ParseLimits& limits) {
  constexpr std::string_view kEnd = "\r\n\r\n";
  size_t head_end = buf.find(kEnd);
  if (head_end == std::string_view::npos) {
    if (buf.size() > limits.max_header_bytes) return too_large("request headers too large");
    ParseResult r;
    r.status = ParseStatus::NeedMore;
    return r;
  }
  if (head_end > limits.max_header_bytes) return too_large("request headers too large");

  std::string_view head = buf.substr(0, head_end);
  size_t line_end = head.find("\r\n");
  std::string_view request_line = head.substr(0, line_end);
  std::string_view header_block =
      (line_end == std::string_view::npos) ? std::string_view() : head.substr(line_end + 2);

  // Request line: METHOD SP target SP HTTP-version, single spaces, nothing else.
  size_t sp1 = request_line.find(' ');
  if (sp1 == std::string_view::npos) return bad("malformed request line");
  size_t sp2 = request_line.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos) return bad("malformed request line");
  std::string_view method = request_line.substr(0, sp1);
  std::string_view target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
  std::string_view version = request_line.substr(sp2 + 1);

  if (!is_token(method)) return bad("invalid method");
  if (!method_allowed(method)) return bad("unsupported method");
  if (version != "HTTP/1.1" && version != "HTTP/1.0") return bad("unsupported HTTP version");

  ParsedRequest req;
  std::string err;
  if (!parse_absolute_url(target, req.host, req.port, req.path, err)) {
    // `err` is one of our fixed strings; safe to return.
    ParseResult r;
    r.status = ParseStatus::Malformed;
    r.reason = err;
    return r;
  }
  req.method = std::string(method);
  req.url = std::string(target);

  if (!header_block.empty()) {
    if (!parse_header_lines(header_block, req.headers, err)) {
      ParseResult r;
      r.status = ParseStatus::Malformed;
      r.reason = err;
      return r;
    }
  }

  if (find_header(req.headers, "transfer-encoding") != nullptr) {
    return bad("Transfer-Encoding on requests is not supported");
  }

  // Content-Length: at most one distinct value.
  unsigned long long content_length = 0;
  bool have_len = false;
  for (const auto& h : req.headers) {
    if (!iequals(h.name, "content-length")) continue;
    unsigned long long v = 0;
    if (!parse_uint(h.value, 1ULL << 62, v)) return bad("invalid Content-Length");
    if (have_len && v != content_length) return bad("conflicting Content-Length headers");
    content_length = v;
    have_len = true;
  }
  if (content_length > limits.max_body_bytes) return too_large("request body too large");

  size_t body_start = head_end + kEnd.size();
  if (buf.size() < body_start + content_length) {
    ParseResult r;
    r.status = ParseStatus::NeedMore;
    return r;
  }
  req.body = std::string(buf.substr(body_start, static_cast<size_t>(content_length)));

  ParseResult r;
  r.status = ParseStatus::Complete;
  r.consumed = body_start + static_cast<size_t>(content_length);
  r.request = std::move(req);
  return r;
}

}  // namespace pg
