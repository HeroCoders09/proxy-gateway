#include "common/http_util.hpp"

#include <algorithm>

namespace pg {

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

std::string_view trim_ows(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool is_tchar(char c) {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
    case '-': case '.': case '^': case '_': case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

bool is_token(std::string_view s) {
  if (s.empty()) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return is_tchar(c); });
}

const std::string* find_header(const Headers& headers, std::string_view name) {
  for (const auto& h : headers) {
    if (iequals(h.name, name)) return &h.value;
  }
  return nullptr;
}

bool parse_header_lines(std::string_view block, Headers& out, std::string& err, size_t max_headers) {
  size_t pos = 0;
  while (pos < block.size()) {
    size_t eol = block.find("\r\n", pos);
    std::string_view line = (eol == std::string_view::npos) ? block.substr(pos) : block.substr(pos, eol - pos);
    pos = (eol == std::string_view::npos) ? block.size() : eol + 2;

    if (line.empty()) {
      err = "empty header line";
      return false;
    }
    if (line.front() == ' ' || line.front() == '\t') {
      err = "obsolete header line folding is not supported";
      return false;
    }
    size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      err = "header line without a name or colon";
      return false;
    }
    std::string_view name = line.substr(0, colon);
    if (!is_token(name)) {
      err = "invalid header name";
      return false;
    }
    std::string_view value = trim_ows(line.substr(colon + 1));
    for (char c : value) {
      unsigned char u = static_cast<unsigned char>(c);
      if ((u < 0x20 && u != '\t') || u == 0x7f) {
        err = "control character in header value";
        return false;
      }
    }
    if (out.size() >= max_headers) {
      err = "too many headers";
      return false;
    }
    out.push_back(Header{std::string(name), std::string(value)});
  }
  return true;
}

bool parse_uint(std::string_view s, unsigned long long max, unsigned long long& out) {
  if (s.empty() || s.size() > 20) return false;
  unsigned long long v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    unsigned digit = static_cast<unsigned>(c - '0');
    if (v > (max - digit) / 10) return false;  // v*10 + digit would exceed max
    v = v * 10 + digit;
  }
  out = v;
  return true;
}

void strip_hop_by_hop(Headers& headers) {
  static const char* kHop[] = {"connection", "keep-alive", "proxy-connection", "proxy-authenticate",
                               "proxy-authorization", "te", "trailer", "transfer-encoding", "upgrade"};
  headers.erase(std::remove_if(headers.begin(), headers.end(),
                               [](const Header& h) {
                                 for (const char* n : kHop) {
                                   if (iequals(h.name, n)) return true;
                                 }
                                 return false;
                               }),
                headers.end());
}

}  // namespace pg
