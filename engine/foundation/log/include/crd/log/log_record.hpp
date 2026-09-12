#pragma once

#include <crd/core/types.hpp>
#include <crd/log/log_level.hpp>

#include <chrono>
#include <source_location>
#include <string_view>

namespace crd::log
{
struct Channel;

// A LogRecord is the bundle of "what was logged".
// It is built once on the producing thread and consumed by sinks.
//
// - In sync mode: lives on the calling thread's stack, message points into a
//   per-thread scratch buffer that is valid for the lifetime of dispatch().
// - In async mode: copied into a queue entry that owns its own message string.
//
// Sinks must NOT retain pointers from this struct after write() returns.
struct LogRecord
{
    LogLevel level;
    const Channel* channel;   // never null in normal flow
    std::source_location loc; // file/line/function/column at call site
    std::chrono::system_clock::time_point time;
    u64 thread_id;            // hash of std::this_thread::get_id()
    std::string_view message; // already-formatted text, no trailing newline
};
} // namespace crd::log
