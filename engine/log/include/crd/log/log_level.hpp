#pragma once

#include <crd/core/types.hpp>

namespace crd::log
{
// Severity scale. Lower = more verbose, higher = more important.
// Values intentionally match CRD_LOG_LEVEL_* in build_config.hpp.
enum class LogLevel : u8
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
    Off = 6 // sentinel: a channel set to Off accepts nothing.
};

// Long human-readable name: "Trace", "Debug", ...
const char* to_string(LogLevel level) noexcept;

// Three-letter form used by formatters: "TRC","DBG","INF","WRN","ERR","CRT","OFF".
const char* to_short_string(LogLevel level) noexcept;

// Parse from a (case-insensitive) string. Returns LogLevel::Off if unknown.
LogLevel from_string(const char* str) noexcept;
} // namespace crd::log
