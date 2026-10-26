#include "logger.hpp"
#include <cctype>
#include <cstdarg>
#include <ctime>

bool Logger::parse_level(const std::string& s, LogLevel& out)
{
    std::string v;
    for (char c : s) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "debug")                     { out = LogLevel::Debug;   return true; }
    if (v == "info")                      { out = LogLevel::Info;    return true; }
    if (v == "warn" || v == "warning")    { out = LogLevel::Warning; return true; }
    if (v == "error")                     { out = LogLevel::Error;   return true; }
    return false;
}

void Logger::log(LogLevel lvl, const char* fmt, ...)
{
    if (!enabled(lvl)) return;

    char msg[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    // Timestamp
    time_t t = time(nullptr);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    std::lock_guard<std::mutex> lock(mutex_);
    if (prefix_.empty())
        fprintf(stderr, "%s %s %s\n", ts, level_tag(lvl), msg);
    else
        fprintf(stderr, "%s %s [%s] %s\n", ts, level_tag(lvl), prefix_.c_str(), msg);

    // Live file sink: "<prefix>_live.log" in the working dir, plain text, flushed every line
    // so the log is always available without redirecting the console (which buffers on Windows).
    if (!file_) {
        std::string path = (prefix_.empty() ? std::string("server") : prefix_) + "_live.log";
        file_ = fopen(path.c_str(), "w");
    }
    if (file_) {
        if (prefix_.empty())
            fprintf(file_, "%s %s %s\n", ts, level_tag_plain(lvl), msg);
        else
            fprintf(file_, "%s %s [%s] %s\n", ts, level_tag_plain(lvl), prefix_.c_str(), msg);
        fflush(file_);
    }
}
