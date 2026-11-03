#pragma once

#include "json.hpp"
#include <sys/stat.h>
#include <cstdlib>
#include "security.hpp"
#include "types.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace gateway {

inline bool parse_routing_policy(const std::string& str, RoutingPolicy& out) {
    if (str == "random") { out = RoutingPolicy::RANDOM; return true; }
    if (str == "round_robin" || str == "round-robin") { out = RoutingPolicy::ROUND_ROBIN; return true; }
    if (str == "least_connections" || str == "least-connections") { out = RoutingPolicy::LEAST_CONNECTIONS; return true; }
    if (str == "lowest_latency" || str == "lowest-latency" || str == "latency") { out = RoutingPolicy::LOWEST_LATENCY; return true; }
    if (str == "weighted") { out = RoutingPolicy::WEIGHTED; return true; }
    return false;
}

inline std::string read_file_contents(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Unable to open config file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

namespace cfgdetail {

using json::Value;

[[noreturn]] inline void bad(const std::string& path, const std::string& msg) {
    throw std::runtime_error("Invalid config: '" + path + "' " + msg);
}

inline const Value* member(const Value& obj, const std::string& key) { return obj.find(key); }

inline const Value& as_object(const Value& v, const std::string& path) {
    if (!v.is_object()) bad(path, std::string("must be an object (found ") + v.type_name() + ")");
    return v;
}

inline double get_num(const Value& v, const std::string& path) {
    if (!v.is_number()) bad(path, std::string("must be a number (found ") + v.type_name() + ")");
    return v.number;
}

inline long long get_int(const Value& v, const std::string& path, long long lo, long long hi) {
    double d = get_num(v, path);
    if (d != std::floor(d)) bad(path, "must be an integer");
    if (d < static_cast<double>(lo) || d > static_cast<double>(hi)) {
        bad(path, "must be between " + std::to_string(lo) + " and " + std::to_string(hi));
    }
    return static_cast<long long>(d);
}

inline std::string get_str(const Value& v, const std::string& path) {
    if (!v.is_string()) bad(path, std::string("must be a string (found ") + v.type_name() + ")");
    return v.str;
}

inline void warn_unknown(const Value& obj, const std::string& path, std::initializer_list<const char*> known) {
    for (const auto& k : obj.keys) {
        bool ok = false;
        for (const char* n : known) if (k == n) ok = true;
        if (!ok) std::cerr << "Config warning: unknown key '" << (path.empty() ? "" : path + ".") << k << "' ignored\n";
    }
}

inline RateLimitConfig parse_rate(const Value& v, const std::string& path) {
    as_object(v, path);
    const Value* r = member(v, "rate");
    const Value* b = member(v, "burst");
    if (!r || !b) bad(path, "requires both 'rate' and 'burst'");
    RateLimitConfig rl;
    rl.rate = get_num(*r, path + ".rate");
    rl.burst = get_num(*b, path + ".burst");
    if (!(rl.rate > 0.0)) bad(path + ".rate", "must be > 0");
    if (!(rl.burst >= 1.0)) bad(path + ".burst", "must be >= 1");
    return rl;
}

}  // namespace cfgdetail

namespace cfgdetail {
// A secret may be written inline ("<key>"), come from an environment variable ("<key>_env") or from a file
// ("<key>_file", surrounding whitespace/newline trimmed). Exactly one source may be given.
// Returns false when none is present. `inline_used` is set when the secret sits in the config file itself.
inline bool read_secret(const json::Value& obj, const std::string& key, const std::string& where, std::string& out,
                        bool& inline_used) {
    auto a = member(obj, key);
    auto b = member(obj, key + "_env");
    auto c = member(obj, key + "_file");
    if ((a ? 1 : 0) + (b ? 1 : 0) + (c ? 1 : 0) > 1)
        bad(where, "give only one of '" + key + "', '" + key + "_env', '" + key + "_file'");
    if (a) { out = get_str(*a, where + "." + key); inline_used = true; return true; }
    if (b) {
        std::string name = get_str(*b, where + "." + key + "_env");
        const char* v = name.empty() ? nullptr : std::getenv(name.c_str());
        if (!v || !*v) bad(where + "." + key + "_env", "environment variable '" + name + "' is not set or empty");
        out = v;
        return true;
    }
    if (c) {
        std::string path = get_str(*c, where + "." + key + "_file");
        struct stat fst{};
        if (::stat(path.c_str(), &fst) == 0 && (fst.st_mode & (S_IRWXG | S_IRWXO)) != 0)
            std::cerr << "[Config] WARNING: secret file " << path << " is accessible by group/others (mode " << std::oct
                      << (fst.st_mode & 0777) << std::dec << "): chmod 600 it\n";
        std::string txt;
        try { txt = read_file_contents(path); } catch (const std::exception&) { bad(where + "." + key + "_file", "cannot read '" + path + "'"); }
        while (!txt.empty() && (txt.back() == '\n' || txt.back() == '\r' || txt.back() == ' ' || txt.back() == '\t')) txt.pop_back();
        if (txt.empty()) bad(where + "." + key + "_file", "'" + path + "' is empty");
        out = txt;
        return true;
    }
    return false;
}
}  // namespace cfgdetail

// Parses configuration text (strict JSON) into `cfg`. Throws std::runtime_error / json::ParseError.
inline void load_config_from_string(const std::string& content, GatewayConfig& cfg) {
    using namespace cfgdetail;
    json::Value root = json::parse(content);
    as_object(root, "<root>");
    warn_unknown(root, "", {"listen_host", "listen_port", "max_queue_size", "max_retries", "policy", "weights",
                            "workers", "timeouts", "limits", "circuit", "default_rate_limit", "rate_limits",
                            "proxies", "probe_target_url", "output", "admin"});

    if (auto v = member(root, "listen_host")) {
        cfg.listen_host = get_str(*v, "listen_host");
        if (cfg.listen_host.empty()) bad("listen_host", "must not be empty");
    }
    if (auto v = member(root, "listen_port")) cfg.listen_port = static_cast<uint16_t>(get_int(*v, "listen_port", 1, 65535));
    if (auto v = member(root, "max_queue_size")) cfg.max_queue_size = static_cast<size_t>(get_int(*v, "max_queue_size", 1, 100000000));
    if (auto v = member(root, "max_retries")) cfg.max_retries = static_cast<int>(get_int(*v, "max_retries", 0, 10));
    if (auto v = member(root, "policy")) {
        std::string s = get_str(*v, "policy");
        if (!parse_routing_policy(s, cfg.policy)) bad("policy", "unknown routing policy '" + s + "'");
    }
    if (auto o = member(root, "weights")) {
        as_object(*o, "weights");
        warn_unknown(*o, "weights", {"w_latency", "w_load", "w_failure"});
        if (auto v = member(*o, "w_latency")) cfg.w_latency = get_num(*v, "weights.w_latency");
        if (auto v = member(*o, "w_load")) cfg.w_load = get_num(*v, "weights.w_load");
        if (auto v = member(*o, "w_failure")) cfg.w_failure = get_num(*v, "weights.w_failure");
        if (cfg.w_latency < 0 || cfg.w_load < 0 || cfg.w_failure < 0) bad("weights", "must be non-negative");
        if (cfg.w_latency + cfg.w_load + cfg.w_failure <= 0) bad("weights", "must not all be zero");
    }
    if (auto o = member(root, "workers")) {
        as_object(*o, "workers");
        warn_unknown(*o, "workers", {"min", "max", "initial", "alpha", "beta", "io_threads"});
        if (auto v = member(*o, "min")) cfg.workers_min = static_cast<int>(get_int(*v, "workers.min", 1, 1000000));
        if (auto v = member(*o, "max")) cfg.workers_max = static_cast<int>(get_int(*v, "workers.max", 1, 1000000));
        if (auto v = member(*o, "initial")) cfg.workers_initial = static_cast<int>(get_int(*v, "workers.initial", 1, 1000000));
        if (auto v = member(*o, "alpha")) cfg.aimd_alpha = static_cast<int>(get_int(*v, "workers.alpha", 1, 100000));
        if (auto v = member(*o, "beta")) cfg.aimd_beta = get_num(*v, "workers.beta");
        if (auto v = member(*o, "io_threads")) cfg.io_threads = static_cast<int>(get_int(*v, "workers.io_threads", 0, 64));
        if (cfg.workers_max < cfg.workers_min) bad("workers", "max must be >= min");
        if (cfg.workers_initial < cfg.workers_min || cfg.workers_initial > cfg.workers_max) {
            bad("workers.initial", "must be between workers.min and workers.max");
        }
        if (!(cfg.aimd_beta > 0.0 && cfg.aimd_beta < 1.0)) bad("workers.beta", "must satisfy 0 < beta < 1");
    }
    if (auto o = member(root, "timeouts")) {
        as_object(*o, "timeouts");
        warn_unknown(*o, "timeouts", {"connect_ms", "response_ms", "probe_interval_ms", "queue_ms",
                                      "client_idle_ms", "client_header_ms", "tunnel_idle_ms", "upstream_idle_ms"});
        if (auto v = member(*o, "connect_ms")) cfg.connect_timeout_ms = static_cast<int>(get_int(*v, "timeouts.connect_ms", 1, 3600000));
        if (auto v = member(*o, "response_ms")) cfg.response_timeout_ms = static_cast<int>(get_int(*v, "timeouts.response_ms", 1, 3600000));
        if (auto v = member(*o, "probe_interval_ms")) cfg.probe_interval_ms = static_cast<int>(get_int(*v, "timeouts.probe_interval_ms", 10, 3600000));
        if (auto v = member(*o, "queue_ms")) cfg.queue_timeout_ms = static_cast<int>(get_int(*v, "timeouts.queue_ms", 0, 3600000));
        if (auto v = member(*o, "client_idle_ms")) cfg.client_idle_timeout_ms = static_cast<int>(get_int(*v, "timeouts.client_idle_ms", 10, 86400000));
        if (auto v = member(*o, "client_header_ms")) cfg.client_header_timeout_ms = static_cast<int>(get_int(*v, "timeouts.client_header_ms", 10, 3600000));
        if (auto v = member(*o, "tunnel_idle_ms")) cfg.tunnel_idle_timeout_ms = static_cast<int>(get_int(*v, "timeouts.tunnel_idle_ms", 10, 86400000));
        if (auto v = member(*o, "upstream_idle_ms")) cfg.upstream_idle_timeout_ms = static_cast<int>(get_int(*v, "timeouts.upstream_idle_ms", 100, 3600000));
    }
    if (auto o = member(root, "limits")) {
        as_object(*o, "limits");
        warn_unknown(*o, "limits", {"max_header_bytes", "max_body_bytes", "max_response_bytes", "max_client_connections",
                                    "pool_max_idle_per_proxy", "pool_max_idle_total"});
        if (auto v = member(*o, "max_header_bytes")) cfg.max_header_bytes = static_cast<size_t>(get_int(*v, "limits.max_header_bytes", 256, 16777216));
        if (auto v = member(*o, "max_body_bytes")) cfg.max_body_bytes = static_cast<size_t>(get_int(*v, "limits.max_body_bytes", 0, 4294967296LL));
        if (auto v = member(*o, "max_response_bytes")) cfg.max_response_bytes = static_cast<size_t>(get_int(*v, "limits.max_response_bytes", 1024, 4294967296LL));
        if (auto v = member(*o, "max_client_connections")) cfg.max_client_connections = static_cast<size_t>(get_int(*v, "limits.max_client_connections", 1, 1000000));
        if (auto v = member(*o, "pool_max_idle_per_proxy")) cfg.pool_max_idle_per_proxy = static_cast<size_t>(get_int(*v, "limits.pool_max_idle_per_proxy", 0, 100000));
        if (auto v = member(*o, "pool_max_idle_total")) cfg.pool_max_idle_total = static_cast<size_t>(get_int(*v, "limits.pool_max_idle_total", 0, 1000000));
    }
    if (auto o = member(root, "circuit")) {
        as_object(*o, "circuit");
        warn_unknown(*o, "circuit", {"failure_threshold", "cooldown_sec"});
        if (auto v = member(*o, "failure_threshold")) cfg.circuit_threshold = static_cast<int>(get_int(*v, "circuit.failure_threshold", 1, 1000000));
        if (auto v = member(*o, "cooldown_sec")) cfg.circuit_cooldown_sec = static_cast<int>(get_int(*v, "circuit.cooldown_sec", 0, 86400));
    }
    if (auto v = member(root, "default_rate_limit")) cfg.default_rate_limit = parse_rate(*v, "default_rate_limit");
    if (auto o = member(root, "rate_limits")) {
        as_object(*o, "rate_limits");
        cfg.domain_rate_limits.clear();
        for (size_t i = 0; i < o->keys.size(); ++i) {
            cfg.domain_rate_limits[to_lower(o->keys[i])] = parse_rate(o->items[i], "rate_limits." + o->keys[i]);
        }
    }
    if (auto a = member(root, "proxies")) {
        if (!a->is_array()) bad("proxies", std::string("must be an array (found ") + a->type_name() + ")");
        cfg.proxies.clear();
        for (size_t i = 0; i < a->items.size(); ++i) {
            std::string p = "proxies[" + std::to_string(i) + "]";
            const auto& e = as_object(a->items[i], p);
            auto h = member(e, "host");
            auto pt = member(e, "port");
            if (!h || !pt) bad(p, "requires 'host' and 'port'");
            std::string host = get_str(*h, p + ".host");
            {   // pasted values often carry stray spaces / tabs / newlines: trim them, then accept only host-name characters
                size_t b = host.find_first_not_of(" \t\r\n"), e2 = host.find_last_not_of(" \t\r\n");
                host = (b == std::string::npos) ? std::string() : host.substr(b, e2 - b + 1);
            }
            if (host.empty()) bad(p + ".host", "must not be empty");
            for (unsigned char ch : host)
                if (!(std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == ':' || ch == '[' || ch == ']'))
                    bad(p + ".host", "contains an invalid character (spaces, '/', '@' or control characters are not allowed): put only the host name or IP here and the port in 'port'");
            if (std::count(host.begin(), host.end(), ':') == 1 && host.front() != '[')
                bad(p + ".host", "looks like \"host:port\": put the port in the separate 'port' field");
            ProxyProtocol proto = ProxyProtocol::HTTP;
            if (auto pr = member(e, "protocol")) {
                std::string v = to_lower(get_str(*pr, p + ".protocol"));
                if (v == "http") proto = ProxyProtocol::HTTP;
                else if (v == "socks5") proto = ProxyProtocol::SOCKS5;
                else bad(p + ".protocol", "must be \"http\" or \"socks5\" (found \"" + v + "\")");
            }
            warn_unknown(e, p, {"host", "port", "protocol", "username", "password", "password_env", "password_file"});
            ProxyEndpointConfig ep{host, static_cast<uint16_t>(get_int(*pt, p + ".port", 1, 65535)), proto, {}, {}};
            auto un = member(e, "username");
            std::string pwv;
            bool pw_inline = false;
            bool has_pw = read_secret(e, "password", p, pwv, pw_inline);
            if (pw_inline) cfg.inline_secrets = true;
            if (un || has_pw) {
                if (proto != ProxyProtocol::SOCKS5) bad(p, "'username'/'password' are only supported with \"protocol\": \"socks5\"");
                if (!un || !has_pw) bad(p, "'username' and 'password' (or password_env / password_file) must be given together");
                ep.username = get_str(*un, p + ".username");
                ep.password = pwv;
                if (ep.username.empty() || ep.username.size() > 255) bad(p + ".username", "must be 1..255 bytes");
                if (ep.password.empty() || ep.password.size() > 255) bad(p + ".password", "must be 1..255 bytes");
            }
            cfg.proxies.push_back(std::move(ep));
        }
    }
    if (cfg.proxies.empty()) bad("proxies", "must list at least one proxy");
    if (auto o = member(root, "admin")) {
        as_object(*o, "admin");
        warn_unknown(*o, "admin", {"token", "token_env", "token_file", "port", "listen_host", "allow_remote"});
        if (auto v = member(*o, "port")) cfg.admin_port = static_cast<uint16_t>(get_int(*v, "admin.port", 0, 65535));
        if (auto v = member(*o, "listen_host")) {
            cfg.admin_host = get_str(*v, "admin.listen_host");
            SockAddr probe;
            if (!parse_ip_literal(cfg.admin_host, 0, probe)) bad("admin.listen_host", "must be an IPv4 or IPv6 address");
        }
        if (auto v = member(*o, "allow_remote")) {
            if (!v->is_bool()) bad("admin.allow_remote", std::string("must be true or false (found ") + v->type_name() + ")");
            cfg.admin_allow_remote = v->boolean;
        }
        bool inl = false;
        if (read_secret(*o, "token", "admin", cfg.admin_token, inl)) {
            if (inl) cfg.inline_secrets = true;
            if (cfg.admin_token.size() < 8 || cfg.admin_token.size() > 1024) bad("admin.token", "must be 8..1024 characters");
            for (unsigned char ch : cfg.admin_token)
                if (ch <= 0x20 || ch >= 0x7f) bad("admin.token", "must only contain printable ASCII without spaces");
        }
    }
    if (cfg.admin_port != 0 && cfg.admin_port == cfg.listen_port) bad("admin.port", "must differ from listen_port");
    if (admin_exposed(cfg) && cfg.admin_token.empty())
        bad("admin", "would be reachable from other machines (non-loopback listen_host / allow_remote): "
                     "an admin 'token' (or token_env / token_file) is required; generate one with: gateway --gen-admin-token FILE");
    if (auto v = member(root, "probe_target_url")) cfg.probe_target_url = get_str(*v, "probe_target_url");
    if (auto o = member(root, "output")) {
        as_object(*o, "output");
        warn_unknown(*o, "output", {"csv_path", "json_path"});
        if (auto v = member(*o, "csv_path")) cfg.csv_output_path = get_str(*v, "output.csv_path");
        if (auto v = member(*o, "json_path")) cfg.json_output_path = get_str(*v, "output.json_path");
    }
}

inline void load_config_from_json(const std::string& path, GatewayConfig& cfg) {
    try {
        load_config_from_string(read_file_contents(path), cfg);
    } catch (const json::ParseError& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
    struct stat st{};
    if (cfg.inline_secrets && ::stat(path.c_str(), &st) == 0 && (st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        std::cerr << "[Config] WARNING: " << path << " contains inline passwords/tokens but is accessible by group/others "
                  << "(mode " << std::oct << (st.st_mode & 0777) << std::dec << "): chmod 600 it, or use password_env / password_file / token_env / token_file\n";
}

inline void print_cli_help(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n"
              << "Options:\n"
              << "  --config <path>    Path to JSON configuration file (default: built-in defaults)\n"
              << "  --policy <name>    Routing policy: random | round_robin | least_connections | latency | weighted\n"
              << "  --workers <num>    Initial/maximum number of concurrently in-flight upstream requests\n"
              << "  --port <num>       Gateway listening TCP port (default: 8000)\n"
              << "  --check-config     Validate configuration, print a summary and exit\n"
              << "  --gen-admin-token [FILE]  Print a random admin token (or write it to a new 0600 FILE) and exit\n"
              << "  --help             Show this help message and exit\n";
}

inline bool parse_cli_int(const std::string& s, int lo, int hi, const char* name, int& out) {
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0' || v < lo || v > hi) {
        std::cerr << "Error: " << name << " must be an integer between " << lo << " and " << hi << "\n";
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

inline bool parse_cli_args(int argc, char* argv[], GatewayConfig& cfg) {
    std::string config_path;
    std::string cli_policy;
    int cli_workers = -1;
    int cli_port = -1;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_cli_help(argv[0]);
            std::exit(0);
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--policy" && i + 1 < argc) {
            cli_policy = argv[++i];
        } else if (arg == "--workers" && i + 1 < argc) {
            if (!parse_cli_int(argv[++i], 1, 1000000, "--workers", cli_workers)) return false;
        } else if (arg == "--port" && i + 1 < argc) {
            if (!parse_cli_int(argv[++i], 1, 65535, "--port", cli_port)) return false;
        } else if (arg == "--check-config") {
            cfg.check_only = true;
        } else {
            std::cerr << "Error: Invalid or incomplete CLI option '" << arg << "'\n";
            print_cli_help(argv[0]);
            return false;
        }
    }

    if (!config_path.empty()) {
        load_config_from_json(config_path, cfg);
    } else if (cfg.proxies.empty()) {
        cfg.proxies = {{"127.0.0.1", 8101}, {"127.0.0.1", 8102}, {"127.0.0.1", 8103}};
    }

    if (!cli_policy.empty() && !parse_routing_policy(cli_policy, cfg.policy)) {
        std::cerr << "Error: Invalid routing policy '" << cli_policy << "'\n";
        return false;
    }
    if (cli_workers != -1) {
        cfg.workers_initial = cli_workers;
        cfg.workers_max = std::max(cfg.workers_max, cli_workers);
        cfg.workers_min = std::min(cfg.workers_min, cli_workers);
    }
    if (cli_port != -1) cfg.listen_port = static_cast<uint16_t>(cli_port);
    return true;
}

} // namespace gateway
