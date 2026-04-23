#ifndef NET_LOG_H
#define NET_LOG_H

#include <atomic>
#include <cstdio>

namespace bknet {

// Runtime-adjustable log level. 0 = errors only, 1 = +warnings (default),
// 2 = +info, 3 = +debug. Set via `set_log_level()` or env `BKNET_LOG_LEVEL`.
enum class LogLevel : int {
    Error = 0,
    Warn  = 1,
    Info  = 2,
    Debug = 3,
};

extern std::atomic<int> g_log_level;

inline void set_log_level(LogLevel lvl) { g_log_level.store(static_cast<int>(lvl)); }
inline int  get_log_level() { return g_log_level.load(); }

} // namespace bknet

// Single-expression log guard. Evaluating the printf only when enabled keeps
// hot paths free of string formatting cost when the level is raised.
#define BKNET_LOG(level, ...)                                                 \
    do {                                                                      \
        if (static_cast<int>(bknet::LogLevel::level) <= bknet::g_log_level.load()) { \
            std::fprintf(stderr, "[bknet:" #level "] " __VA_ARGS__);          \
            std::fprintf(stderr, "\n");                                       \
        }                                                                     \
    } while (0)

#endif // NET_LOG_H
