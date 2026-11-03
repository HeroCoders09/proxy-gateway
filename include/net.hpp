#pragma once
// Socket helpers shared by the gateway, proxy_node, target_server and loadgen:
// deadline-based poll waits, send_all, non-blocking connect, HTTP response reader, bidirectional relay.

#include "http.hpp"
#include "types.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace gateway {

using NetClock = std::chrono::steady_clock;

inline NetClock::time_point deadline_in(int timeout_ms) {
    return NetClock::now() + std::chrono::milliseconds(timeout_ms);
}

inline void set_nonblocking(int fd, bool nb = true) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    ::fcntl(fd, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

inline void set_nodelay(int fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// Wait until `fd` is ready for `events` or the deadline passes.
// Returns 1 when ready (or error/hangup: the next syscall reports it), 0 on timeout, -1 on invalid fd.
inline int wait_fd_until(int fd, short events, NetClock::time_point deadline) {
    for (;;) {
        auto now = NetClock::now();
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (ms < 0) ms = 0;
        if (ms > 1000000) ms = 1000000;
        pollfd p{fd, events, 0};
        int rc = ::poll(&p, 1, static_cast<int>(ms));
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) return 0;
        if (p.revents & POLLNVAL) return -1;
        return 1;
    }
}

inline bool send_all(int fd, const char* data, size_t len, int timeout_ms) {
    auto deadline = deadline_in(timeout_ms);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd_until(fd, POLLOUT, deadline) != 1) return false;
            continue;
        }
        return false;
    }
    return true;
}

inline bool send_all(int fd, const std::string& s, int timeout_ms) {
    return send_all(fd, s.data(), s.size(), timeout_ms);
}

// Sends everything immediately or reports failure (never waits). For small replies written from event loops.
inline bool send_nowait(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;  // EAGAIN (client not reading) or error
    }
    return true;
}

// recv with timeout. Returns bytes read, 0 on EOF, -1 on error, -2 on timeout.
inline ssize_t recv_some(int fd, char* buf, size_t len, int timeout_ms) {
    auto deadline = deadline_in(timeout_ms);
    for (;;) {
        ssize_t n = ::recv(fd, buf, len, 0);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int w = wait_fd_until(fd, POLLIN, deadline);
            if (w == 0) return -2;
            if (w < 0) return -1;
            continue;
        }
        return -1;
    }
}

// ---------------------------------------------------------------- addresses (IPv4 + IPv6)
inline void sockaddr_set_port(SockAddr& a, uint16_t port) {
    if (a.family() == AF_INET6) reinterpret_cast<sockaddr_in6*>(&a.ss)->sin6_port = htons(port);
    else reinterpret_cast<sockaddr_in*>(&a.ss)->sin_port = htons(port);
}

inline uint16_t sockaddr_get_port(const sockaddr_storage& ss) {
    if (ss.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_port);
    return ntohs(reinterpret_cast<const sockaddr_in*>(&ss)->sin_port);
}

inline std::string strip_brackets(const std::string& h) {
    return (h.size() >= 2 && h.front() == '[' && h.back() == ']') ? h.substr(1, h.size() - 2) : h;
}

// IPv4 / IPv6 literal -> SockAddr (no DNS, never blocks). "[::1]" and "::1" are both accepted.
inline bool parse_ip_literal(const std::string& host_in, uint16_t port, SockAddr& out) {
    std::string host = strip_brackets(host_in);
    SockAddr a;
    in_addr a4{};
    in6_addr a6{};
    if (::inet_pton(AF_INET, host.c_str(), &a4) == 1) {
        auto* p = reinterpret_cast<sockaddr_in*>(&a.ss);
        p->sin_family = AF_INET;
        p->sin_addr = a4;
        a.len = sizeof(sockaddr_in);
    } else if (::inet_pton(AF_INET6, host.c_str(), &a6) == 1) {
        auto* p = reinterpret_cast<sockaddr_in6*>(&a.ss);
        p->sin6_family = AF_INET6;
        p->sin6_addr = a6;
        a.len = sizeof(sockaddr_in6);
    } else {
        return false;
    }
    sockaddr_set_port(a, port);
    out = a;
    return true;
}

// Text form of the IP part (no port); used for de-duplication and logs.
inline std::string addr_to_string_raw(const SockAddr& a) {
    char b[INET6_ADDRSTRLEN] = {0};
    if (a.family() == AF_INET6) ::inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(&a.ss)->sin6_addr, b, sizeof(b));
    else ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&a.ss)->sin_addr, b, sizeof(b));
    return b;
}

// Literal or host name -> ALL addresses (both families, resolver order, duplicates removed, at most `max_addrs`).
// Host-name lookups BLOCK: never call from an event loop.
inline bool resolve_all(const std::string& host_in, uint16_t port, std::vector<SockAddr>& out, size_t max_addrs = 4) {
    out.clear();
    SockAddr lit;
    if (parse_ip_literal(host_in, port, lit)) { out.push_back(lit); return true; }
    std::string host = strip_brackets(host_in);
    for (int attempt = 0; attempt < 2 && out.empty(); ++attempt) {   // AI_ADDRCONFIG hides loopback-only hosts: retry without it
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (attempt == 0) hints.ai_flags = AI_ADDRCONFIG;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) continue;
        for (addrinfo* ai = res; ai && out.size() < max_addrs; ai = ai->ai_next) {
            if ((ai->ai_family != AF_INET && ai->ai_family != AF_INET6) || ai->ai_addrlen > sizeof(sockaddr_storage)) continue;
            SockAddr a;
            std::memcpy(&a.ss, ai->ai_addr, ai->ai_addrlen);
            a.len = static_cast<socklen_t>(ai->ai_addrlen);
            sockaddr_set_port(a, port);
            bool dup = false;
            for (const auto& o : out)
                if (o.family() == a.family() && addr_to_string_raw(o) == addr_to_string_raw(a)) { dup = true; break; }
            if (!dup) out.push_back(a);
        }
        ::freeaddrinfo(res);
    }
    return !out.empty();
}

// First address only (callers that cannot fail over, e.g. security classification).
inline bool resolve_addr(const std::string& host_in, uint16_t port, SockAddr& out) {
    std::vector<SockAddr> v;
    if (!resolve_all(host_in, port, v, 1)) return false;
    out = v.front();
    return true;
}

inline std::string addr_to_string(const SockAddr& a) {
    char b[INET6_ADDRSTRLEN] = {0};
    if (a.family() == AF_INET6) ::inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(&a.ss)->sin6_addr, b, sizeof(b));
    else ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&a.ss)->sin_addr, b, sizeof(b));
    return b;
}

// Opens a blocking listening socket ("0.0.0.0" = all IPv4, "::" = all IPv6 AND IPv4 / dual stack). Returns fd or -1.
inline int listen_on(const std::string& host, uint16_t port, int backlog, bool nonblocking = false) {
    SockAddr a;
    if (!parse_ip_literal(host, port, a)) return -1;
    int fd = ::socket(a.family(), SOCK_STREAM | SOCK_CLOEXEC | (nonblocking ? SOCK_NONBLOCK : 0), 0);
    if (fd < 0) return -1;
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (a.family() == AF_INET6) {
        int off = 0;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));   // "::" also accepts IPv4 clients
    }
    if (::bind(fd, a.sa(), a.len) < 0 || ::listen(fd, backlog) < 0) { int e = errno; ::close(fd); errno = e; return -1; }
    return fd;
}

// Tries the addresses in order (e.g. "localhost" = ::1 then 127.0.0.1) until one connects; `timeout_ms` is the TOTAL
// budget. Returns a NON-BLOCKING fd or -1; `used` receives the index of the address that worked.
inline int connect_addrs(const std::vector<SockAddr>& addrs, int timeout_ms, bool& timed_out, size_t* used = nullptr) {
    timed_out = false;
    auto deadline = deadline_in(timeout_ms);
    for (size_t i = 0; i < addrs.size(); ++i) {
        const SockAddr& addr = addrs[i];
        int fd = ::socket(addr.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        set_nodelay(fd);
        int rc = ::connect(fd, addr.sa(), addr.len);
        bool ok = rc == 0;
        if (rc < 0 && errno == EINPROGRESS) {
            int w = wait_fd_until(fd, POLLOUT, deadline);
            if (w == 0) { timed_out = true; ::close(fd); return -1; }   // budget spent
            int so_err = 0;
            socklen_t sl = sizeof(so_err);
            ok = w > 0 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &sl) == 0 && so_err == 0;
        }
        if (ok) { if (used) *used = i; return fd; }
        ::close(fd);   // refused / unreachable: next address
    }
    return -1;
}

// Resolves `host` and connects with fallback across all of its addresses.
// (Blocking helper for tools and tests; the gateway itself uses the event-driven engine.)
inline int connect_with_timeout(const std::string& host, uint16_t port, int timeout_ms, bool& timed_out) {
    timed_out = false;
    std::vector<SockAddr> addrs;
    if (!resolve_all(host, port, addrs)) return -1;
    return connect_addrs(addrs, timeout_ms, timed_out);
}

// ------------------------------------------------------------ response reading
enum class ReadStatus { Ok, Timeout, Error, TooLarge, NeedMore };

struct ResponseData {
    std::string raw;           // complete HTTP message exactly as received (headers + body)
    int status = 0;
    bool framed = false;       // length known (Content-Length / chunked / no-body status)
    bool close_header = false; // response says "Connection: close" (or is HTTP/1.0 without keep-alive)
    bool peer_closed = false;  // upstream closed the TCP connection
    bool extra_bytes = false;  // bytes beyond the framed message were received
    bool any_bytes = false;    // at least one byte was received
};

// Incremental HTTP response parser: feed() bytes as they arrive (any chunking), it reports when the
// message is complete. Used by the event-driven upstream engine and by the blocking reader below.
class ResponseParser {
public:
    ResponseParser(bool head_only, size_t max_bytes) : head_only_(head_only), max_bytes_(max_bytes) {}

    ResponseData data;

    ReadStatus feed(const char* d, size_t n) {
        data.raw.append(d, n);
        if (n) data.any_bytes = true;
        if (data.raw.size() > max_bytes_ + 65536) return ReadStatus::TooLarge;
        return evaluate(false);
    }

    // The peer closed the connection: either completes a close-delimited body or is an error.
    ReadStatus finish_eof() {
        data.peer_closed = true;
        ReadStatus s = evaluate(true);
        return s == ReadStatus::NeedMore ? ReadStatus::Error : s;
    }

private:
    ReadStatus evaluate(bool eof) {
        std::string& buf = data.raw;
        while (header_end_ == std::string::npos) {
            size_t he = buf.find("\r\n\r\n");
            if (he == std::string::npos) break;
            size_t eol = buf.find("\r\n");
            std::string status_line = buf.substr(0, eol);
            int status = 0;
            if (!parse_status_line(status_line.data(), status_line.size(), status)) return ReadStatus::Error;
            if (status >= 100 && status < 200 && status != 101) {  // interim response: drop and keep reading
                buf.erase(0, he + 4);
                continue;
            }
            HeaderList hl;
            std::string err;
            std::string block = (eol < he) ? buf.substr(eol + 2, he - (eol + 2)) : std::string();
            if (!parse_header_lines(block, hl, err)) return ReadStatus::Error;
            data.status = status;
            header_end_ = he;
            no_body_ = head_only_ || status == 204 || status == 304 || (status >= 100 && status < 200);
            data.close_header = (status_line.compare(0, 8, "HTTP/1.0") == 0);
            if (const std::string* c = header_find(hl, "connection")) {
                if (header_has_token(*c, "close")) data.close_header = true;
                else if (header_has_token(*c, "keep-alive")) data.close_header = false;
            }
            if (!no_body_) {
                if (const std::string* te = header_find(hl, "transfer-encoding")) {
                    if (header_has_token(*te, "chunked")) chunked_ = true;
                }
                if (!chunked_) {
                    for (const auto& kv : hl) {
                        if (!iequals(kv.first, "content-length")) continue;
                        uint64_t v = 0;
                        if (!parse_uint(kv.second, v)) return ReadStatus::Error;
                        if (have_cl_ && v != content_length_) return ReadStatus::Error;
                        content_length_ = static_cast<size_t>(v);
                        have_cl_ = true;
                    }
                }
            }
        }

        if (header_end_ != std::string::npos) {
            size_t body_start = header_end_ + 4;
            if (no_body_) {
                data.framed = true;
                data.extra_bytes = buf.size() > body_start;
                if (data.extra_bytes) buf.resize(body_start);
                return ReadStatus::Ok;
            }
            if (chunked_) {
                // Incremental: resume at the first incomplete chunk (earlier ones are already validated), so a body
                // arriving in many small reads is scanned in linear time. Handles trailers and bytes after the end.
                size_t used = 0, resume = 0;
                size_t off = body_start + chunk_off_;
                ChunkStatus cs = scan_chunked(buf.data() + off, buf.size() - off, nullptr, used, SIZE_MAX, &resume);
                if (cs == ChunkStatus::Complete) {
                    size_t end = off + used;
                    data.framed = true;
                    data.extra_bytes = end < buf.size();
                    if (data.extra_bytes) buf.resize(end);
                    return ReadStatus::Ok;
                }
                if (cs == ChunkStatus::Bad || cs == ChunkStatus::TooLarge) return ReadStatus::Error;
                chunk_off_ += resume;
                if (eof) return ReadStatus::Error;
            } else if (have_cl_) {
                size_t total = body_start + content_length_;
                if (total > max_bytes_) return ReadStatus::TooLarge;
                if (buf.size() >= total) {
                    data.framed = true;
                    data.extra_bytes = buf.size() > total;
                    if (data.extra_bytes) buf.resize(total);
                    return ReadStatus::Ok;
                }
                if (eof) return ReadStatus::Error;  // truncated body
            } else if (eof) {
                data.framed = false;  // body delimited by connection close
                return ReadStatus::Ok;
            }
        } else if (eof) {
            return ReadStatus::Error;
        }
        if (buf.size() > max_bytes_) return ReadStatus::TooLarge;
        return ReadStatus::NeedMore;
    }

    bool head_only_;
    size_t max_bytes_;
    size_t header_end_ = std::string::npos;
    bool chunked_ = false, have_cl_ = false, no_body_ = false;
    size_t content_length_ = 0;
    size_t chunk_off_ = 0;   // offset (from the end of the headers) of the first not-yet-validated chunk
};

// Blocking reader (tools/tests): reads one complete HTTP response from `fd` within `timeout_ms` overall.
// `head_only` = response to a HEAD request (never has a body).
inline ReadStatus read_http_response(int fd, bool head_only, int timeout_ms, size_t max_bytes, ResponseData& out) {
    auto deadline = deadline_in(timeout_ms);
    ResponseParser parser(head_only, max_bytes);
    auto done = [&](ReadStatus s) { out = std::move(parser.data); return s; };
    for (;;) {
        int w = wait_fd_until(fd, POLLIN, deadline);
        if (w == 0) return done(ReadStatus::Timeout);
        if (w < 0) return done(ReadStatus::Error);
        char chunk[16384];
        for (;;) {
            ssize_t n = ::recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
            if (n > 0) {
                ReadStatus st = parser.feed(chunk, static_cast<size_t>(n));
                if (st != ReadStatus::NeedMore) return done(st);
                continue;
            }
            if (n == 0) return done(parser.finish_eof());
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return done(ReadStatus::Error);
        }
    }
}

// ------------------------------------------------------------------ SOCKS5 client
// RFC 1928, "no authentication" + CONNECT only. Domain names are sent to the proxy (it resolves them), IPv4
// literals are sent as addresses. The pure helpers below are shared by the event-driven engine and the prober.
// Offers "no authentication" and, when credentials are configured, RFC 1929 username/password as well.
inline std::string socks5_greeting(bool offer_userpass = false) {
    return offer_userpass ? std::string("\x05\x02\x00\x02", 4) : std::string("\x05\x01\x00", 3);
}

// RFC 1929 sub-negotiation request. Both fields must be 1..255 bytes.
inline bool socks5_auth_request(const std::string& user, const std::string& pass, std::string& out) {
    if (user.empty() || user.size() > 255 || pass.empty() || pass.size() > 255) return false;
    out.assign("\x01", 1);
    out += static_cast<char>(user.size());
    out += user;
    out += static_cast<char>(pass.size());
    out += pass;
    return true;
}

// Sub-negotiation reply: 1 = accepted, 0 = need more bytes, -1 = rejected / malformed.
inline int socks5_parse_auth_reply(const std::string& b) {
    if (!b.empty() && static_cast<unsigned char>(b[0]) != 1) return -1;
    if (b.size() < 2) return 0;
    return b[1] == 0 ? 1 : -1;
}

inline bool socks5_connect_request(const std::string& host, uint16_t port, std::string& out) {
    out.assign("\x05\x01\x00", 3);
    in_addr a4{};
    in6_addr a6{};
    if (::inet_pton(AF_INET, host.c_str(), &a4) == 1) {
        out += '\x01';
        out.append(reinterpret_cast<const char*>(&a4), 4);
    } else if (::inet_pton(AF_INET6, host.c_str(), &a6) == 1) {
        out += '\x04';
        out.append(reinterpret_cast<const char*>(&a6), 16);
    } else {
        if (host.empty() || host.size() > 255 || host.find(':') != std::string::npos) return false;  // malformed literal
        out += '\x03';
        out += static_cast<char>(host.size());
        out += host;
    }
    out += static_cast<char>(port >> 8);
    out += static_cast<char>(port & 0xff);
    return true;
}

// Method-selection reply: 1 = "no auth" chosen, 2 = username/password chosen, 0 = need more bytes,
// -1 = refused (0xFF) / unknown method / malformed.
inline int socks5_parse_method_reply(const std::string& b) {
    if (b.empty()) return 0;
    if (static_cast<unsigned char>(b[0]) != 5) return -1;
    if (b.size() < 2) return 0;
    if (b[1] == 0) return 1;
    return b[1] == 2 ? 2 : -1;
}

// CONNECT reply: 1 = succeeded (`total` bytes belong to the reply), 0 = need more bytes,
// -1 = failure / malformed (`rep` = SOCKS reply code, 0xFF when malformed).
inline int socks5_parse_reply(const std::string& b, size_t& total, int& rep) {
    rep = 0xFF;
    if (!b.empty() && static_cast<unsigned char>(b[0]) != 5) return -1;
    if (b.size() < 2) return 0;
    rep = static_cast<unsigned char>(b[1]);
    if (rep != 0) return -1;
    if (b.size() < 4) return 0;
    size_t addr = 0;
    switch (static_cast<unsigned char>(b[3])) {
        case 0x01: addr = 4; break;
        case 0x04: addr = 16; break;
        case 0x03:
            if (b.size() < 5) return 0;
            addr = 1 + static_cast<unsigned char>(b[4]);
            break;
        default: rep = 0xFF; return -1;
    }
    total = 4 + addr + 2;
    return b.size() >= total ? 1 : 0;
}

// Blocking handshake for tools / the prober thread (fd may be non-blocking; every step is bounded by `timeout_ms`).
inline const char* socks5_reply_text(int rep) {
    switch (rep) {
        case 1: return "general SOCKS server failure";
        case 2: return "connection not allowed by the proxy's ruleset (blocked destination or port)";
        case 3: return "network unreachable (from the proxy)";
        case 4: return "host unreachable (the proxy could not resolve or reach the target)";
        case 5: return "connection refused by the target (from the proxy)";
        case 6: return "TTL expired";
        case 7: return "command not supported";
        case 8: return "address type not supported";
        default: return "malformed reply (this is probably not a SOCKS5 server: wrong protocol or port?)";
    }
}

// `why` (optional) receives a human-readable failure reason.
inline bool socks5_handshake_blocking(int fd, const std::string& host, uint16_t port, int timeout_ms,
                                      const std::string& user = std::string(), const std::string& pass = std::string(),
                                      std::string* why = nullptr) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    std::string req, in;
    bool creds = !user.empty() || !pass.empty();
    if (!socks5_connect_request(host, port, req)) return fail("target host name is too long for SOCKS5");
    if (!send_all(fd, socks5_greeting(creds), timeout_ms)) return fail("proxy closed the connection during the SOCKS5 greeting");
    char tmp[512];
    for (;;) {
        int st = socks5_parse_method_reply(in);
        if (st < 0) {
            if (!in.empty() && static_cast<unsigned char>(in[0]) != 5)
                return fail("not a SOCKS5 server (wrong protocol? it may be HTTP or SOCKS4)");
            return fail(creds ? "proxy rejected every authentication method we offered (check username/password)"
                              : "proxy requires authentication: enter a username and password");
        }
        if (st > 0) {
            if (st == 2) {   // server picked username/password
                std::string ar;
                if (!creds) return fail("proxy requires authentication: enter a username and password");
                if (!socks5_auth_request(user, pass, ar) || !send_all(fd, ar, timeout_ms)) return fail("could not send credentials");
                std::string ain;
                for (;;) {
                    int as = socks5_parse_auth_reply(ain);
                    if (as < 0) return fail("wrong username or password (rejected by the proxy)");
                    if (as > 0) break;
                    ssize_t m = recv_some(fd, tmp, 2 - std::min<size_t>(ain.size(), 2), timeout_ms);
                    if (m <= 0) return fail("proxy closed the connection or timed out during authentication");
                    ain.append(tmp, static_cast<size_t>(m));
                }
            }
            break;
        }
        ssize_t n = recv_some(fd, tmp, 2 - std::min<size_t>(in.size(), 2), timeout_ms);
        if (n <= 0) return fail("no SOCKS5 reply (timeout or closed): not a SOCKS5 server, or it is dead");
        in.append(tmp, static_cast<size_t>(n));
    }
    if (!send_all(fd, req, timeout_ms)) return fail("proxy closed the connection before the CONNECT request");
    in.clear();
    for (;;) {
        size_t total = 0;
        int rep = 0;
        int st = socks5_parse_reply(in, total, rep);
        if (st < 0) { if (why) *why = std::string("SOCKS5 CONNECT failed: ") + socks5_reply_text(rep); return false; }
        if (st > 0) return true;
        ssize_t n = recv_some(fd, tmp, 1, timeout_ms);   // byte-wise: never reads past the reply
        if (n <= 0) return fail("proxy did not answer the CONNECT request (timeout or closed)");
        in.append(tmp, static_cast<size_t>(n));
    }
}

// ----------------------------------------------------------------------- relay
// Copies bytes both ways between two sockets until both directions finish (EOF + flush),
// an error occurs, the connection is idle for `idle_ms`, or `*running` becomes false (server shutdown).
inline void relay_bidirectional(int a, int b, int idle_ms, const std::atomic<bool>* running) {
    set_nonblocking(a);
    set_nonblocking(b);
    struct Dir {
        int src, dst;
        std::string buf;
        size_t off = 0;
        bool src_eof = false, shut = false;
        bool pending() const { return off < buf.size(); }
        bool finished() const { return shut; }
    };
    Dir d1{a, b, {}, 0, false, false}, d2{b, a, {}, 0, false, false};
    auto last = NetClock::now();

    auto maybe_shutdown = [](Dir& d) {
        if (d.src_eof && !d.pending() && !d.shut) {
            ::shutdown(d.dst, SHUT_WR);
            d.shut = true;
        }
    };
    auto pump_in = [&](Dir& d) -> bool {
        char tmp[16384];
        ssize_t n = ::recv(d.src, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n > 0) { d.buf.append(tmp, static_cast<size_t>(n)); last = NetClock::now(); return true; }
        if (n == 0) { d.src_eof = true; maybe_shutdown(d); return true; }
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    };
    auto pump_out = [&](Dir& d) -> bool {
        ssize_t n = ::send(d.dst, d.buf.data() + d.off, d.buf.size() - d.off, MSG_NOSIGNAL);
        if (n > 0) {
            d.off += static_cast<size_t>(n);
            last = NetClock::now();
            if (d.off >= d.buf.size()) { d.buf.clear(); d.off = 0; maybe_shutdown(d); }
            return true;
        }
        return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    };

    while (!(d1.finished() && d2.finished())) {
        if (running && !running->load(std::memory_order_relaxed)) return;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(NetClock::now() - last).count() > idle_ms) return;
        pollfd p[2] = {{a, 0, 0}, {b, 0, 0}};
        if (!d1.src_eof && !d1.pending()) p[0].events |= POLLIN;
        if (d2.pending()) p[0].events |= POLLOUT;
        if (!d2.src_eof && !d2.pending()) p[1].events |= POLLIN;
        if (d1.pending()) p[1].events |= POLLOUT;
        int rc = ::poll(p, 2, 500);
        if (rc < 0) { if (errno == EINTR) continue; return; }
        if (rc == 0) continue;
        for (auto& pf : p) if (pf.revents & POLLNVAL) return;
        // a: readable -> d1 input, writable -> d2 output
        if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) && (p[0].events & POLLIN)) { if (!pump_in(d1)) return; }
        if ((p[1].revents & (POLLIN | POLLHUP | POLLERR)) && (p[1].events & POLLIN)) { if (!pump_in(d2)) return; }
        if ((p[0].revents & (POLLOUT | POLLERR | POLLHUP)) && (p[0].events & POLLOUT) && d2.pending()) { if (!pump_out(d2)) return; }
        if ((p[1].revents & (POLLOUT | POLLERR | POLLHUP)) && (p[1].events & POLLOUT) && d1.pending()) { if (!pump_out(d1)) return; }
        // Error with nothing else to do on that fd -> bail out
        if ((p[0].revents & POLLERR) || (p[1].revents & POLLERR)) return;
    }
}

}  // namespace gateway
