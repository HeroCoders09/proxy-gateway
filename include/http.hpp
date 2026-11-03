#pragma once
// HTTP/1.x message helpers: incremental request parsing (headers + Content-Length / chunked bodies),
// URL/authority parsing, response framing helpers and log-safe escaping.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace gateway {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// ---------------------------------------------------------------- string utils
inline std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

inline bool iequals(const std::string& a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

inline bool is_token_char(unsigned char c) {
    if (std::isalnum(c)) return true;
    return std::strchr("!#$%&'*+-.^_`|~", c) != nullptr && c != 0;
}

inline bool is_token(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!is_token_char(c)) return false;
    return true;
}

inline const std::string* header_find(const HeaderList& h, const char* name) {
    for (const auto& kv : h) {
        if (iequals(kv.first, name)) return &kv.second;
    }
    return nullptr;
}

// True if the comma-separated header value contains `token` (case-insensitive).
inline bool header_has_token(const std::string& value, const char* token) {
    size_t i = 0;
    while (i <= value.size()) {
        size_t j = value.find(',', i);
        if (j == std::string::npos) j = value.size();
        if (iequals(trim(value.substr(i, j - i)), token)) return true;
        i = j + 1;
    }
    return false;
}

inline bool is_hop_by_hop(const std::string& lname) {
    static const char* kNames[] = {"connection", "proxy-connection", "keep-alive", "proxy-authenticate",
                                   "proxy-authorization", "te", "trailer", "transfer-encoding",
                                   "upgrade", "expect", "content-length"};
    for (const char* n : kNames) if (lname == n) return true;
    return false;
}

inline bool parse_uint(const std::string& s, uint64_t& out, size_t max_digits = 18) {
    if (s.empty() || s.size() > max_digits) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    out = v;
    return true;
}

// --------------------------------------------------------------- header parsing
// `block` holds header lines separated by CRLF (no terminating blank line).
inline bool parse_header_lines(const std::string& block, HeaderList& out, std::string& err) {
    size_t pos = 0;
    while (pos < block.size()) {
        size_t eol = block.find("\r\n", pos);
        if (eol == std::string::npos) eol = block.size();
        std::string line = block.substr(pos, eol - pos);
        pos = eol + 2;
        if (line.empty()) continue;
        if (line[0] == ' ' || line[0] == '\t') { err = "obsolete header line folding is not supported"; return false; }
        size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) { err = "malformed header line"; return false; }
        std::string name = line.substr(0, colon);
        if (!is_token(name)) { err = "invalid header name"; return false; }
        std::string value = trim(line.substr(colon + 1));
        for (unsigned char c : value) {
            if ((c < 0x20 && c != '\t') || c == 0x7f) { err = "control character in header value"; return false; }
        }
        out.emplace_back(std::move(name), std::move(value));
    }
    return true;
}

// ------------------------------------------------------------ chunked decoding
enum class ChunkStatus { Complete, NeedMore, Bad, TooLarge };

inline size_t find_crlf(const char* d, size_t from, size_t n) {
    while (from + 1 < n) {
        const void* p = std::memchr(d + from, '\r', n - from - 1);
        if (!p) return std::string::npos;
        size_t idx = static_cast<size_t>(static_cast<const char*>(p) - d);
        if (d[idx + 1] == '\n') return idx;
        from = idx + 1;
    }
    return std::string::npos;
}

// Scans (and optionally decodes into `out`) a chunked body starting at d[0].
// `resume` (optional): on NeedMore, the offset of the first incomplete chunk. Callers that receive the body piecemeal can
// restart the scan there (only valid with out == nullptr), so the total work stays linear in the body size.
inline ChunkStatus scan_chunked(const char* d, size_t n, std::string* out, size_t& consumed, size_t max_out,
                                size_t* resume = nullptr) {
    size_t pos = 0, total = 0, chunk_start = 0;
    auto need_more = [&]() { if (resume) *resume = chunk_start; return ChunkStatus::NeedMore; };
    for (;;) {
        chunk_start = pos;
        size_t eol = find_crlf(d, pos, n);
        if (eol == std::string::npos) return (n - pos > 1024) ? ChunkStatus::Bad : need_more();
        size_t size = 0, digits = 0;
        size_t i = pos;
        for (; i < eol && d[i] != ';'; ++i) {
            char c = d[i];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return ChunkStatus::Bad;
            if (++digits > 15) return ChunkStatus::Bad;
            size = (size << 4) | static_cast<size_t>(v);
        }
        if (digits == 0) return ChunkStatus::Bad;
        pos = eol + 2;
        if (size == 0) {
            for (;;) {  // optional trailers, ends with an empty line
                size_t e2 = find_crlf(d, pos, n);
                if (e2 == std::string::npos) return (n - pos > 8192) ? ChunkStatus::Bad : need_more();
                if (e2 == pos) { consumed = e2 + 2; return ChunkStatus::Complete; }
                pos = e2 + 2;
            }
        }
        if (size > max_out || total + size > max_out) return ChunkStatus::TooLarge;
        if (n - pos < size + 2) return need_more();
        if (d[pos + size] != '\r' || d[pos + size + 1] != '\n') return ChunkStatus::Bad;
        if (out) out->append(d + pos, size);
        total += size;
        pos += size + 2;
    }
}

// ---------------------------------------------------------------- request parse
enum class ParseStatus { NeedMore, Complete, Bad };

struct HttpRequest {
    std::string method, target, version;
    HeaderList headers;
    std::string body;
    bool keep_alive = true;
};

struct ParseResult {
    ParseStatus status = ParseStatus::NeedMore;
    int error_status = 400;
    std::string error;
    bool want_continue = false;  // client sent "Expect: 100-continue" and the body is still missing
    size_t consumed = 0;
    HttpRequest req;
};

inline ParseResult parse_bad(int status, const std::string& msg) {
    ParseResult r;
    r.status = ParseStatus::Bad;
    r.error_status = status;
    r.error = msg;
    return r;
}

// Parses at most one request from the front of `buf`. Never throws.
inline ParseResult parse_http_request(const std::string& buf, size_t max_header, size_t max_body) {
    size_t start = 0;
    while (start < buf.size() && (buf[start] == '\r' || buf[start] == '\n')) ++start;  // tolerate stray CRLF
    size_t hend = buf.find("\r\n\r\n", start);
    if (hend == std::string::npos) {
        if (buf.size() - start > max_header) return parse_bad(431, "request header too large");
        ParseResult r;
        r.status = ParseStatus::NeedMore;
        return r;
    }
    if (hend - start > max_header) return parse_bad(431, "request header too large");

    size_t line_end = buf.find("\r\n", start);
    if (line_end == std::string::npos || line_end > hend) line_end = hend;
    std::string line = buf.substr(start, line_end - start);

    HttpRequest req;
    {
        size_t s1 = line.find(' ');
        size_t s2 = (s1 == std::string::npos) ? std::string::npos : line.find(' ', s1 + 1);
        if (s1 == std::string::npos || s2 == std::string::npos || s1 == 0 || s2 == s1 + 1) {
            return parse_bad(400, "malformed request line");
        }
        req.method = line.substr(0, s1);
        req.target = line.substr(s1 + 1, s2 - s1 - 1);
        req.version = line.substr(s2 + 1);
        if (!is_token(req.method)) return parse_bad(400, "invalid method");
        for (unsigned char c : req.target) {
            if (c <= 0x20 || c == 0x7f) return parse_bad(400, "invalid character in request target");
        }
        if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0") return parse_bad(400, "unsupported HTTP version");
    }

    if (line_end < hend) {
        std::string block = buf.substr(line_end + 2, hend - (line_end + 2));
        std::string err;
        if (!parse_header_lines(block, req.headers, err)) return parse_bad(400, err);
    }

    // Connection persistence
    bool is11 = (req.version == "HTTP/1.1");
    req.keep_alive = is11;
    for (const char* name : {"connection", "proxy-connection"}) {
        if (const std::string* v = header_find(req.headers, name)) {
            if (header_has_token(*v, "close")) req.keep_alive = false;
            else if (header_has_token(*v, "keep-alive")) req.keep_alive = true;
        }
    }

    // Body framing
    size_t body_start = hend + 4;
    const std::string* te = header_find(req.headers, "transfer-encoding");
    bool want_continue = false;
    if (const std::string* ex = header_find(req.headers, "expect")) {
        want_continue = is11 && header_has_token(*ex, "100-continue");
    }

    size_t consumed = body_start;
    std::string cl_value;
    bool have_cl = false;
    for (const auto& kv : req.headers) {
        if (iequals(kv.first, "content-length")) {
            if (have_cl && kv.second != cl_value) return parse_bad(400, "conflicting Content-Length headers");
            cl_value = kv.second;
            have_cl = true;
        }
    }

    if (te) {
        if (have_cl) return parse_bad(400, "both Transfer-Encoding and Content-Length present");
        if (!iequals(trim(*te), "chunked")) return parse_bad(501, "unsupported Transfer-Encoding");
        size_t used = 0;
        std::string decoded;
        ChunkStatus cs = scan_chunked(buf.data() + body_start, buf.size() - body_start, &decoded, used, max_body);
        if (cs == ChunkStatus::NeedMore) {
            ParseResult r;
            r.status = ParseStatus::NeedMore;
            r.want_continue = want_continue;
            return r;
        }
        if (cs == ChunkStatus::TooLarge) return parse_bad(413, "request body too large");
        if (cs == ChunkStatus::Bad) return parse_bad(400, "malformed chunked body");
        req.body = std::move(decoded);
        consumed = body_start + used;
    } else if (have_cl) {
        uint64_t len = 0;
        if (!parse_uint(cl_value, len)) return parse_bad(400, "invalid Content-Length");
        if (len > max_body) return parse_bad(413, "request body too large");
        if (buf.size() - body_start < len) {
            ParseResult r;
            r.status = ParseStatus::NeedMore;
            r.want_continue = want_continue;
            return r;
        }
        req.body = buf.substr(body_start, static_cast<size_t>(len));
        consumed = body_start + static_cast<size_t>(len);
    }

    ParseResult r;
    r.status = ParseStatus::Complete;
    r.consumed = consumed;
    r.req = std::move(req);
    return r;
}

// ------------------------------------------------------------------- URL parse
// authority := host [":" port] | "[" ipv6 "]" [":" port]
inline bool parse_authority(const std::string& a, std::string& host, uint16_t& port, int default_port) {
    if (a.empty() || a.find('@') != std::string::npos) return false;
    std::string port_str;
    bool has_port = false;
    if (a[0] == '[') {
        size_t rb = a.find(']');
        if (rb == std::string::npos || rb == 1) return false;
        host = a.substr(1, rb - 1);
        if (rb + 1 < a.size()) {
            if (a[rb + 1] != ':') return false;
            port_str = a.substr(rb + 2);
            has_port = true;
        }
    } else {
        size_t colon = a.find(':');
        if (colon != std::string::npos) {
            if (a.find(':', colon + 1) != std::string::npos) return false;  // bare IPv6 needs brackets
            host = a.substr(0, colon);
            port_str = a.substr(colon + 1);
            has_port = true;
        } else {
            host = a;
        }
    }
    if (host.empty()) return false;
    for (unsigned char c : host) {
        if (c <= 0x20 || c == 0x7f || c == '/' || c == '?' || c == '#' || c == '\\') return false;
    }
    if (has_port) {
        uint64_t p = 0;
        if (!parse_uint(port_str, p, 5) || p == 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p);
    } else {
        if (default_port < 0) return false;
        port = static_cast<uint16_t>(default_port);
    }
    return true;
}

struct UrlParts {
    bool https = false;
    std::string authority;  // as written, lower-cased
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
};

inline bool parse_absolute_url(const std::string& url, UrlParts& out) {
    std::string rest;
    if (url.size() > 7 && iequals(url.substr(0, 7), "http://")) {
        rest = url.substr(7);
        out.https = false;
    } else if (url.size() > 8 && iequals(url.substr(0, 8), "https://")) {
        rest = url.substr(8);
        out.https = true;
    } else {
        return false;
    }
    size_t end = rest.find_first_of("/?#");
    std::string authority = (end == std::string::npos) ? rest : rest.substr(0, end);
    std::string tail = (end == std::string::npos) ? "" : rest.substr(end);
    if (!parse_authority(authority, out.host, out.port, out.https ? 443 : 80)) return false;
    out.authority = to_lower(authority);
    size_t hash = tail.find('#');
    if (hash != std::string::npos) tail.erase(hash);
    if (tail.empty()) tail = "/";
    else if (tail[0] == '?') tail = "/" + tail;
    out.path = tail;
    return true;
}

// Strict HTTP/1.x status line: "HTTP/1.<digit> SP 3DIGIT [SP reason]" (no CR/LF; `n` = length of the line).
// Rejects e.g. "HTTP/1HTTP/1.1 200", "HTTP/1.1 2000", "HTTP/1.1  200", "HTTP/2 200". Status must be 100..599.
inline bool parse_status_line(const char* s, size_t n, int& status) {
    if (n < 12 || std::memcmp(s, "HTTP/1.", 7) != 0) return false;
    if (s[7] < '0' || s[7] > '9' || s[8] != ' ') return false;
    if (s[9] < '0' || s[9] > '9' || s[10] < '0' || s[10] > '9' || s[11] < '0' || s[11] > '9') return false;
    if (n > 12 && s[12] != ' ') return false;
    int v = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    if (v < 100 || v > 599) return false;
    status = v;
    return true;
}

// ------------------------------------------------------------ response helpers
inline const char* http_status_text(int s) {
    switch (s) {
        case 100: return "Continue";
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
    }
}

inline std::string build_response(int status, const std::string& content_type, const std::string& body,
                                  bool keep_alive, bool head_only = false, const std::string& extra_headers = std::string()) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + http_status_text(status) + "\r\n";
    out += "Content-Type: " + content_type + "\r\n";
    out += extra_headers;   // complete "Name: value\r\n" lines
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    if (!head_only) out += body;
    return out;
}

// ------------------------------------------------------------- log-safe escapes
// Length of a valid UTF-8 sequence starting at s[i], or 0 if invalid.
inline size_t utf8_seq_len(const std::string& s, size_t i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = 0;
    uint32_t cp = 0;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; }
    else return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return 0;
    if (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;
    return n;
}

// CSV field: always quoted, quotes doubled, control characters and invalid UTF-8 bytes rendered as \xNN
// so that one record == one line and the file stays valid UTF-8.
inline std::string csv_escape(const std::string& s) {
    static const char* hex = "0123456789abcdef";
    std::string out = "\"";
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"') { out += "\"\""; ++i; }
        else if (c < 0x20 || c == 0x7f) { out += "\\x"; out += hex[c >> 4]; out += hex[c & 15]; ++i; }
        else if (c < 0x80) { out += static_cast<char>(c); ++i; }
        else {
            size_t n = utf8_seq_len(s, i);
            if (n == 0) { out += "\\x"; out += hex[c >> 4]; out += hex[c & 15]; ++i; }
            else { out.append(s, i, n); i += n; }
        }
    }
    out += '"';
    return out;
}

// JSON string body (without surrounding quotes). Invalid UTF-8 becomes U+FFFD.
inline std::string json_escape(const std::string& s) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"') { out += "\\\""; ++i; }
        else if (c == '\\') { out += "\\\\"; ++i; }
        else if (c == '\n') { out += "\\n"; ++i; }
        else if (c == '\r') { out += "\\r"; ++i; }
        else if (c == '\t') { out += "\\t"; ++i; }
        else if (c < 0x20 || c == 0x7f) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; ++i; }
        else if (c < 0x80) { out += static_cast<char>(c); ++i; }
        else {
            size_t n = utf8_seq_len(s, i);
            if (n == 0) { out += "\\ufffd"; ++i; }
            else { out.append(s, i, n); i += n; }
        }
    }
    return out;
}

// Prometheus label value escaping.
inline std::string label_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

}  // namespace gateway
