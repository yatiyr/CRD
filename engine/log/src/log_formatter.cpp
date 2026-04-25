#include "log_formatter.hpp"

#include <crd/log/log_channel.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <thread>

namespace crd::log::detail
{
namespace
{
// ANSI color escapes per level.
const char* color_for(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Trace:
            return "\x1b[38;5;245m"; // grey
        case LogLevel::Debug:
            return "\x1b[36m"; // cyan
        case LogLevel::Info:
            return "\x1b[32m"; // green
        case LogLevel::Warn:
            return "\x1b[33m"; // yellow
        case LogLevel::Error:
            return "\x1b[31m"; // red
        case LogLevel::Critical:
            return "\x1b[1;41;97m"; // bold red bg + bright white fg
        default:
            return "";
    }
}

constexpr const char* kReset = "\x1b[0m";
} // namespace

const char* basename_of(const char* path) noexcept
{
    if (!path)
    {
        return "";
    }
    const char* last = path;
    for (const char* p = path; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            last = p + 1;
        }
    }
    return last;
}

u64 current_thread_id() noexcept
{
    // Thread-local cache so we hash std::thread::id only once per thread.
    static thread_local u64 cached = []
    {
        const std::hash<std::thread::id> hasher;
        return static_cast<u64>(hasher(std::this_thread::get_id()));
    }();
    return cached;
}

std::string format_record(const LogRecord& rec, bool with_color, bool short_path)
{
    // Timestamp components.
    const auto sys_time = std::chrono::system_clock::to_time_t(rec.time);
    const auto subsecond = std::chrono::duration_cast<std::chrono::milliseconds>(rec.time.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#if defined(_WIN32)
    ::localtime_s(&tm_buf, &sys_time);
#else
    ::localtime_r(&sys_time, &tm_buf);
#endif

    const char* level_short = to_short_string(rec.level);
    const char* channel_name = (rec.channel && rec.channel->name) ? rec.channel->name : "?";
    const char* file_str = rec.loc.file_name() ? rec.loc.file_name() : "";
    if (short_path)
    {
        file_str = basename_of(file_str);
    }

    const char* color_open = with_color ? color_for(rec.level) : "";
    const char* color_close = with_color ? kReset : "";

    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} {}[{}]{} [{}] tid={} {}:{} - {}",
                       tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min,
                       tm_buf.tm_sec, static_cast<int>(subsecond.count()), color_open, level_short, color_close,
                       channel_name, rec.thread_id, file_str, static_cast<unsigned>(rec.loc.line()), rec.message);
}
} // namespace crd::log::detail
