#include "config.hpp"
#include "gateway_server.hpp"
#include "fd_budget.hpp"
#include <atomic>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <sys/resource.h>
#include <thread>

namespace {
std::atomic<bool> g_shutdown{false};
void signal_handler(int) {
    g_shutdown.store(true);
}

// Many concurrent client + upstream sockets: raise the soft fd limit to the hard limit.
void raise_fd_limit() {
    rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        ::setrlimit(RLIMIT_NOFILE, &rl);
    }
}

// `gateway --gen-admin-token [FILE]`: 256 random bits from the kernel CSPRNG, hex encoded. With FILE the token is
// written to a NEW file created with mode 0600 (never overwrites); otherwise it is printed to stdout.
int generate_admin_token(const char* file) {
    unsigned char raw[32];
    int rfd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t got = 0;
    while (rfd >= 0 && got < sizeof(raw)) {
        ssize_t n = ::read(rfd, raw + got, sizeof(raw) - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    if (rfd >= 0) ::close(rfd);
    if (got != sizeof(raw)) { std::cerr << "cannot read /dev/urandom\n"; return 1; }
    static const char* hex = "0123456789abcdef";
    std::string token;
    for (unsigned char b : raw) { token += hex[b >> 4]; token += hex[b & 15]; }
    std::memset(raw, 0, sizeof(raw));
    if (!file) { std::cout << token << "\n"; return 0; }
    int fd = ::open(file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { std::perror(file); return 1; }
    ::fchmod(fd, 0600);   // independent of the umask
    std::string line = token + "\n";
    bool ok = ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
    ::close(fd);
    if (!ok) { std::cerr << "cannot write " << file << "\n"; return 1; }
    std::cout << "Admin token written to " << file << " (mode 600). Use it with \"admin\": {\"token_file\": \"" << file << "\"}\n";
    return 0;
}
} // namespace

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--gen-admin-token")
            return generate_admin_token(i + 1 < argc && argv[i + 1][0] != '-' ? argv[i + 1] : nullptr);
    gateway::GatewayConfig cfg;
    try {
        if (!gateway::parse_cli_args(argc, argv, cfg)) {
            return 1;
        }
    } catch (const std::exception& ex) {
        std::cerr << "Configuration Error: " << ex.what() << "\n";
        return 1;
    }

    for (const auto& w : gateway::security_warnings(cfg)) std::cerr << "[Security] WARNING: " << w << "\n";

    if (cfg.check_only) {
        std::cout << "Configuration OK: port " << cfg.listen_port << ", policy "
                  << gateway::policy_to_string(cfg.policy) << ", workers [" << cfg.workers_min << ".."
                  << cfg.workers_max << " init=" << cfg.workers_initial << "], proxies " << cfg.proxies.size()
                  << ", rate-limited domains " << cfg.domain_rate_limits.size() << "\n";
        return 0;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
    raise_fd_limit();
    {
        rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0)
            for (const auto& n : gateway::apply_fd_budget(cfg, static_cast<long>(rl.rlim_cur)))
                std::cerr << "[Limits] " << n << "\n";
    }

    gateway::GatewayServer server(cfg);
    if (!server.start()) {
        std::cerr << "Failed to start Intelligent Concurrent Proxy Gateway on " << cfg.listen_host << ":"
                  << cfg.listen_port << "\n";
        return 1;
    }

    std::cout << "[Gateway] Listening on port " << server.port()
              << " | Policy: " << gateway::policy_to_string(cfg.policy)
              << " | Workers: [" << cfg.workers_min << ".." << cfg.workers_max
              << " (init=" << cfg.workers_initial << ")]"
              << " | Proxies: " << cfg.proxies.size() << std::endl;

    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n[Gateway] Shutting down cleanly..." << std::endl;
    server.stop();
    return 0;
}
