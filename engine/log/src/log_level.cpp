#include <crd/log/log_level.hpp>

#include <cstring>

namespace crd::log
{
namespace
{
// Case-insensitive byte compare. Sufficient for ASCII level names.
bool ieq(const char* a, const char* b) noexcept
{
    if (!a || !b)
    {
        return false;
    }
    while (*a && *b)
    {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
        {
            ca = static_cast<char>(ca + ('a' - 'A'));
        }
        if (cb >= 'A' && cb <= 'Z')
        {
            cb = static_cast<char>(cb + ('a' - 'A'));
        }
        if (ca != cb)
        {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}
} // namespace

const char* to_string(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Trace:
            return "Trace";
        case LogLevel::Debug:
            return "Debug";
        case LogLevel::Info:
            return "Info";
        case LogLevel::Warn:
            return "Warn";
        case LogLevel::Error:
            return "Error";
        case LogLevel::Critical:
            return "Critical";
        case LogLevel::Off:
            return "Off";
    }
    return "Unknown";
}

const char* to_short_string(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Trace:
            return "TRC";
        case LogLevel::Debug:
            return "DBG";
        case LogLevel::Info:
            return "INF";
        case LogLevel::Warn:
            return "WRN";
        case LogLevel::Error:
            return "ERR";
        case LogLevel::Critical:
            return "CRT";
        case LogLevel::Off:
            return "OFF";
    }
    return "???";
}

LogLevel from_string(const char* str) noexcept
{
    if (!str)
    {
        return LogLevel::Off;
    }
    if (ieq(str, "trace"))
    {
        return LogLevel::Trace;
    }
    if (ieq(str, "debug"))
    {
        return LogLevel::Debug;
    }
    if (ieq(str, "info"))
    {
        return LogLevel::Info;
    }
    if (ieq(str, "warn") || ieq(str, "warning"))
    {
        return LogLevel::Warn;
    }
    if (ieq(str, "error"))
    {
        return LogLevel::Error;
    }
    if (ieq(str, "critical") || ieq(str, "fatal"))
    {
        return LogLevel::Critical;
    }
    if (ieq(str, "off") || ieq(str, "none"))
    {
        return LogLevel::Off;
    }
    return LogLevel::Off;
}
} // namespace crd::log
