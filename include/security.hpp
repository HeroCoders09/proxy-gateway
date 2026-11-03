#pragma once
// Network-trust helpers and start-up security warnings.
#include "net.hpp"
#include "types.hpp"
#include <string>
#include <vector>

namespace gateway {

// IPv4 address (host byte order) of a socket address, including IPv4-mapped IPv6 (::ffff:a.b.c.d).
inline bool sockaddr_v4(const sockaddr_storage& ss, uint32_t& out) {
    if (ss.ss_family == AF_INET) {
        out = ntohl(reinterpret_cast<const sockaddr_in*>(&ss)->sin_addr.s_addr);
        return true;
    }
    if (ss.ss_family == AF_INET6) {
        const in6_addr& a = reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(&a)) {
            uint32_t v;
            std::memcpy(&v, &a.s6_addr[12], 4);
            out = ntohl(v);
            return true;
        }
    }
    return false;
}

inline bool addr_is_loopback(const sockaddr_storage& ss) {
    uint32_t v4;
    if (sockaddr_v4(ss, v4)) return (v4 >> 24) == 127;
    if (ss.ss_family == AF_INET6) return IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_addr);
    return false;
}

// Loopback, RFC 1918 private, link-local, CGNAT / overlay (100.64/10, e.g. Tailscale) and IPv6 ULA / link-local:
// networks where an unencrypted credential is not exposed to the public internet.
inline bool addr_is_private(const sockaddr_storage& ss) {
    if (addr_is_loopback(ss)) return true;
    uint32_t v4;
    if (sockaddr_v4(ss, v4)) {
        return (v4 >> 24) == 10 || (v4 >> 20) == 0xAC1 /*172.16/12*/ || (v4 >> 16) == 0xC0A8 /*192.168/16*/ ||
               (v4 >> 16) == 0xA9FE /*169.254/16*/ || (v4 >> 22) == (0x64400000u >> 22) /*100.64/10*/;
    }
    if (ss.ss_family == AF_INET6) {
        const in6_addr& a = reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_addr;
        return (a.s6_addr[0] & 0xFE) == 0xFC || (a.s6_addr[0] == 0xFE && (a.s6_addr[1] & 0xC0) == 0x80);
    }
    return false;
}

// True for a literal loopback address or "localhost" (no DNS involved).
inline bool host_is_loopback_literal(const std::string& host) {
    if (host == "localhost") return true;
    SockAddr a;
    return parse_ip_literal(host, 0, a) && addr_is_loopback(a.ss);
}

// Is the admin interface reachable from other machines?
inline bool admin_exposed(const GatewayConfig& cfg) {
    return cfg.admin_port > 0 ? !host_is_loopback_literal(cfg.admin_host) : cfg.admin_allow_remote;
}

// Human-readable warnings about risky-but-allowed setups (printed at start-up and by --check-config).
// Resolves proxy host names, so call it from start-up code only, never from an I/O path.
inline std::vector<std::string> security_warnings(const GatewayConfig& cfg) {
    std::vector<std::string> w;
    for (const auto& ep : cfg.proxies) {
        if (ep.protocol != ProxyProtocol::SOCKS5) continue;
        SockAddr a;
        if (!resolve_addr(ep.host, ep.port, a) || addr_is_private(a.ss)) continue;
        std::string who = (ep.host.find(':') != std::string::npos ? "[" + ep.host + "]" : ep.host) + ":" + std::to_string(ep.port);
        bool creds = !ep.username.empty() || !ep.password.empty();
        w.push_back("SOCKS5 proxy " + who + " (" + addr_to_string(a) + ") is outside loopback/private networks: " +
                    (creds ? "its username/password (RFC 1929) and all traffic cross the network UNENCRYPTED"
                           : "it is unauthenticated and all traffic crosses the network UNENCRYPTED") +
                    ". Reach it through WireGuard / an SSH tunnel or a private network.");
    }
    if (!host_is_loopback_literal(cfg.listen_host)) {
        w.push_back("proxy listener " + cfg.listen_host + ":" + std::to_string(cfg.listen_port) +
                    " accepts non-local clients and the forward proxy has NO client authentication: anyone who can reach it can use it "
                    "(an open proxy). Restrict it with a firewall / private network, or listen on 127.0.0.1.");
    }
    if (admin_exposed(cfg)) {
        std::string where = cfg.admin_port > 0 ? "admin interface " + cfg.admin_host + ":" + std::to_string(cfg.admin_port)
                                               : "admin endpoints on the proxy port";
        w.push_back(where + " accept non-local clients over plain HTTP: the admin token travels unencrypted. "
                            "Publish it only on a private interface, or put a TLS reverse proxy / VPN / SSH tunnel in front.");
    }
    return w;
}

}  // namespace gateway
