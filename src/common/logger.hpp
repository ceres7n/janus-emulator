#pragma once
#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>
#include <mutex>

// ─────────────────────────────────────────────────────────────────────────────
//  Janus Logger — simple thread-safe console/file logger
//  Similar to rAthena's ShowInfo/ShowWarning/ShowError macros
//
//  Level policy (conf key log_level: debug | info | warn | error, default info):
//    INFO  = lifecycle (start/stop, config, listen, connect/disconnect, login/logout, char enter/leave,
//            zone change), GM command use, refusals of suspicious requests, economy audit lines.
//    DEBUG = per-packet, per-move, per-hit, per-tick, per-effect-state chatter. Anything costly built
//            only for a debug line (a hex dump) goes inside `if (Logger::instance().enabled(LogLevel::Debug))`.
// ─────────────────────────────────────────────────────────────────────────────

enum class LogLevel { Debug, Info, Warning, Error, Fatal };

class Logger
{
public:
    static Logger& instance()
    {
        static Logger inst;
        return inst;
    }

    void set_level(LogLevel lvl)  { min_level_.store(lvl, std::memory_order_relaxed); }
    LogLevel level() const        { return min_level_.load(std::memory_order_relaxed); }
    bool enabled(LogLevel lvl) const { return lvl >= level(); }
    void set_prefix(const char* p){ prefix_ = p; }

    // "debug" | "info" | "warn" (or "warning") | "error", any case. False (out untouched) for anything else.
    static bool parse_level(const std::string& s, LogLevel& out);

    // A client-supplied string (an account or character name) as it may go into the log: control
    // bytes (escape sequences that would drive the operator's console) become '?'.
    static std::string printable(const std::string& s)
    {
        std::string out(s);
        for (char& c : out)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = '?';
        return out;
    }

#if defined(__GNUC__) || defined(__clang__)
    void log(LogLevel lvl, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
#else
    void log(LogLevel lvl, const char* fmt, ...);
#endif

    // Convenience wrappers
    template<typename... Args>
    void debug(const char* fmt, Args&&... args)   { log(LogLevel::Debug,   fmt, std::forward<Args>(args)...); }
    template<typename... Args>
    void info(const char* fmt, Args&&... args)    { log(LogLevel::Info,    fmt, std::forward<Args>(args)...); }
    template<typename... Args>
    void warn(const char* fmt, Args&&... args)    { log(LogLevel::Warning, fmt, std::forward<Args>(args)...); }
    template<typename... Args>
    void error(const char* fmt, Args&&... args)   { log(LogLevel::Error,   fmt, std::forward<Args>(args)...); }
    template<typename... Args>
    void fatal(const char* fmt, Args&&... args)   { log(LogLevel::Fatal,   fmt, std::forward<Args>(args)...); }

private:
    Logger() = default;
    std::atomic<LogLevel> min_level_{ LogLevel::Info };   // the mains set it from log_level
    std::string prefix_;
    std::mutex  mutex_;
    FILE*       file_{ nullptr };   // live file sink (flushed every line)

    static const char* level_tag_plain(LogLevel lvl)
    {
        switch (lvl) {
        case LogLevel::Debug:   return "[DEBUG]";
        case LogLevel::Info:    return "[INFO ]";
        case LogLevel::Warning: return "[WARN ]";
        case LogLevel::Error:   return "[ERROR]";
        case LogLevel::Fatal:   return "[FATAL]";
        }
        return "[?????]";
    }

    static const char* level_tag(LogLevel lvl)
    {
        switch (lvl) {
        case LogLevel::Debug:   return "\033[36m[DEBUG]\033[0m";
        case LogLevel::Info:    return "\033[32m[INFO ]\033[0m";
        case LogLevel::Warning: return "\033[33m[WARN ]\033[0m";
        case LogLevel::Error:   return "\033[31m[ERROR]\033[0m";
        case LogLevel::Fatal:   return "\033[35m[FATAL]\033[0m";
        }
        return "[?????]";
    }
};

// Global shorthand macros (rAthena-style). LOG_DEBUG checks the level first, so its arguments are
// not even evaluated when debug is off (it stays an expression, usable wherever the call was).
#define LOG_DEBUG(...)   (Logger::instance().enabled(LogLevel::Debug) ? Logger::instance().debug(__VA_ARGS__) : (void)0)
#define LOG_INFO(...)    Logger::instance().info(__VA_ARGS__)
#define LOG_WARN(...)    Logger::instance().warn(__VA_ARGS__)
#define LOG_ERROR(...)   Logger::instance().error(__VA_ARGS__)
#define LOG_FATAL(...)   Logger::instance().fatal(__VA_ARGS__)
