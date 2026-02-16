#pragma once
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <ctime>

namespace dedup {

enum class LogLevel : int { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

inline LogLevel g_log_level = LogLevel::INFO;

inline void log(LogLevel level, const char* fmt, ...) {
    if (level < g_log_level) return;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) .count() % 1000;

    struct tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    const char* prefix = "???";
    switch (level) {
        case LogLevel::DEBUG: prefix = "DBG"; break;
        case LogLevel::INFO:  prefix = "INF"; break;
        case LogLevel::WARN:  prefix = "WRN"; break;
        case LogLevel::ERROR: prefix = "ERR"; break;
    }

    std::fprintf(stderr, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] ",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, static_cast<int>(ms),
        prefix);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

#define LOG_DBG(...) ::dedup::log(::dedup::LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INF(...) ::dedup::log(::dedup::LogLevel::INFO,  __VA_ARGS__)
#define LOG_WRN(...) ::dedup::log(::dedup::LogLevel::WARN,  __VA_ARGS__)
#define LOG_ERR(...) ::dedup::log(::dedup::LogLevel::ERROR, __VA_ARGS__)

} // namespace dedup
