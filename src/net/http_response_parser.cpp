#include "net/http_response_parser.hpp"

#include "common/http_util.hpp"

namespace pg {

namespace {

RespParseResult make(RespStatus s, const char* err = "") {
  RespParseResult r;
  r.status = s;
  r.error = err;
  return r;
}

// Decode a chunked body starting at `data`. Returns Complete (and fills out/consumed),
// NeedMore, or Malformed.
RespStatus decode_chunked(std::string_view data, std::string& out, size_t& consumed, size_t max_body) {
  size_t pos = 0;
  out.clear();
  for (;;) {
    size_t eol = data.find("\r\n", pos);
    if (eol == std::string_view::npos) return data.size() - pos > 1024 ? RespStatus::Malformed : RespStatus::NeedMore;
    std::string_view size_line = data.substr(pos, eol - pos);
    size_t semi = size_line.find(';');  // ignore chunk extensions
    if (semi != std::string_view::npos) size_line = size_line.substr(0, semi);
    size_line = trim_ows(size_line);
    if (size_line.empty() || size_line.size() > 16) return RespStatus::Malformed;
    unsigned long long sz = 0;
    for (char c : size_line) {
      unsigned d;
      if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
      else return RespStatus::Malformed;
      sz = sz * 16 + d;
    }
    pos = eol + 2;
    if (sz == 0) {
      // Trailer section: zero or more header lines, then an empty line.
      for (;;) {
        size_t e2 = data.find("\r\n", pos);
        if (e2 == std::string_view::npos) return RespStatus::NeedMore;
        if (e2 == pos) {
          consumed = e2 + 2;
          return RespStatus::Complete;
        }
        pos = e2 + 2;
      }
    }
    if (sz > max_body || out.size() + sz > max_body) return RespStatus::Malformed;
    if (data.size() < pos + sz + 2) return RespStatus::NeedMore;
    out.append(data.substr(pos, static_cast<size_t>(sz)));
    if (data.substr(pos + sz, 2) != "\r\n") return RespStatus::Malformed;
    pos += static_cast<size_t>(sz) + 2;
  }
}

}  // namespace

RespParseResult parse_response(std::string_view buf, bool head_request, bool eof, size_t max_total) {
  if (buf.size() > max_total) return make(RespStatus::TooLarge, "response too large");

  size_t offset = 0;
  ParsedResponse resp;

  for (;;) {  // loop only to skip interim 1xx responses
    std::string_view rest = buf.substr(offset);
    size_t head_end = rest.find("\r\n\r\n");
    if (head_end == std::string_view::npos) {
      if (rest.size() > 65536) return make(RespStatus::Malformed, "response headers too large");
      if (eof) return make(RespStatus::Malformed, "connection closed before response headers completed");
      return make(RespStatus::NeedMore);
    }
    std::string_view head = rest.substr(0, head_end);
    size_t line_end = head.find("\r\n");
    std::string_view status_line = head.substr(0, line_end);
    std::string_view header_block =
        (line_end == std::string_view::npos) ? std::string_view() : head.substr(line_end + 2);

    // "HTTP/1.x SP 3DIGIT [SP reason]"
    if (status_line.size() < 12 || status_line.substr(0, 7) != "HTTP/1." ||
        (status_line[7] != '0' && status_line[7] != '1') || status_line[8] != ' ') {
      return make(RespStatus::Malformed, "invalid status line");
    }
    unsigned long long code = 0;
    if (!parse_uint(status_line.substr(9, 3), 999, code) || code < 100) {
      return make(RespStatus::Malformed, "invalid status code");
    }
    if (status_line.size() > 12 && status_line[12] != ' ') return make(RespStatus::Malformed, "invalid status line");

    ParsedResponse cur;
    cur.status = static_cast<int>(code);
    if (status_line.size() > 13) cur.reason = std::string(status_line.substr(13));
    std::string err;
    if (!header_block.empty() && !parse_header_lines(header_block, cur.headers, err, 200)) {
      return make(RespStatus::Malformed, "invalid response headers");
    }

    size_t body_start = offset + head_end + 4;
    if (cur.status >= 100 && cur.status < 200 && cur.status != 101) {
      offset = body_start;  // skip interim response, parse the next one
      continue;
    }

    const std::string* conn = find_header(cur.headers, "connection");
    cur.upstream_wants_close = (conn != nullptr && iequals(trim_ows(*conn), "close")) ||
                               status_line[7] == '0';

    std::string_view body_data = buf.substr(body_start);
    const bool no_body = head_request || cur.status == 204 || cur.status == 304 || cur.status == 101;
    if (no_body) {
      RespParseResult r;
      r.status = RespStatus::Complete;
      r.response = std::move(cur);
      return r;
    }

    const std::string* te = find_header(cur.headers, "transfer-encoding");
    if (te != nullptr) {
      std::string v = to_lower(trim_ows(*te));
      // "chunked" must be the final coding; we only support exactly "chunked".
      if (v != "chunked") return make(RespStatus::Malformed, "unsupported transfer coding");
      size_t consumed = 0;
      RespStatus st = decode_chunked(body_data, cur.body, consumed, max_total);
      if (st == RespStatus::Malformed) return make(RespStatus::Malformed, "invalid chunked body");
      if (st == RespStatus::NeedMore) {
        if (eof) return make(RespStatus::Malformed, "connection closed inside chunked body");
        return make(RespStatus::NeedMore);
      }
      RespParseResult r;
      r.status = RespStatus::Complete;
      r.response = std::move(cur);
      return r;
    }

    // Content-Length (all copies must agree).
    bool have_len = false;
    unsigned long long clen = 0;
    for (const auto& h : cur.headers) {
      if (!iequals(h.name, "content-length")) continue;
      unsigned long long v = 0;
      if (!parse_uint(trim_ows(h.value), 1ULL << 62, v)) return make(RespStatus::Malformed, "invalid Content-Length");
      if (have_len && v != clen) return make(RespStatus::Malformed, "conflicting Content-Length headers");
      clen = v;
      have_len = true;
    }
    if (have_len) {
      if (clen > max_total) return make(RespStatus::TooLarge, "response too large");
      if (body_data.size() < clen) {
        if (eof) return make(RespStatus::Malformed, "connection closed before full body arrived");
        return make(RespStatus::NeedMore);
      }
      cur.body = std::string(body_data.substr(0, static_cast<size_t>(clen)));
      RespParseResult r;
      r.status = RespStatus::Complete;
      r.response = std::move(cur);
      return r;
    }

    // No framing: body runs until the peer closes.
    if (!eof) return make(RespStatus::NeedMore);
    cur.body = std::string(body_data);
    RespParseResult r;
    r.status = RespStatus::Complete;
    r.response = std::move(cur);
    return r;
  }
}

}  // namespace pg
