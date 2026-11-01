#pragma once
// Minimal stderr logger. Rule (SRS security): never pass credentials or response bodies here.
#include <cstdio>
#include <ctime>
#include <string>

namespace pg {

inline void log_line(const char* level, const std::string& msg) {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char ts[32];
  std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
  std::fprintf(stderr, "%s [%s] %s\n", ts, level, msg.c_str());
}

inline void log_info(const std::string& msg) { log_line("INFO", msg); }
inline void log_warn(const std::string& msg) { log_line("WARN", msg); }
inline void log_error(const std::string& msg) { log_line("ERROR", msg); }

}  // namespace pg
