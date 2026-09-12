#pragma once

#include <crd/log/log_record.hpp>

#include <string>

namespace crd::log::detail
{
// Renders one record into a human-readable single line:
//   YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [Channel] tid=NNNN file:line - message
//
// 'with_color' adds ANSI escape sequences around the [LEVEL] tag (and resets at end).
// 'short_path' strips directory prefix from the source file for readability.
std::string format_record(const LogRecord& rec, bool with_color, bool short_path);

// Just the basename portion of a file path: last '/' or '\\' tail.
const char* basename_of(const char* path) noexcept;

// Cached cross-platform thread id hash. Cheap to call.
u64 current_thread_id() noexcept;
} // namespace crd::log::detail
