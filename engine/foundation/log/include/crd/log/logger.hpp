#pragma once

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/log/log_channel.hpp>
#include <crd/log/log_level.hpp>
#include <crd/log/log_sink.hpp>

#include <memory>
#include <source_location>
#include <string_view>

namespace crd::log
{
struct LoggerConfig
{
    // If true, writes go through a queue + worker thread (non-blocking on producer).
    bool async = false;

    // Async ring capacity. Should be a power of two. Items beyond this are dropped
    // (and an internal counter is incremented) when drop_on_overflow is true,
    // otherwise the producer waits.
    usize async_queue_capacity = 8192;

    // True: drop on overflow (game thread never blocks).
    // False: block until worker drains.
    bool drop_on_overflow = true;

    // If true, a Critical record bypasses the async queue and is delivered + flushed
    // synchronously, so a crashing program still gets its last words to disk.
    bool flush_on_critical = true;
};

// ----- Lifecycle (called from main / engine init) ----------------------
void init(const LoggerConfig& cfg = {}) noexcept;
void shutdown() noexcept;
bool is_initialized() noexcept;

// ----- Sink management -------------------------------------------------
// Takes ownership. Adding sinks before init() is allowed; they are kept until shutdown.
void add_sink(std::unique_ptr<ISink> sink) noexcept;
void clear_sinks() noexcept;

// Block until all queued records are written and sinks are flushed.
void flush() noexcept;

// Number of records dropped because the async queue was full.
u64 dropped_count() noexcept;

// ----- Internal: used by macros only -----------------------------------
namespace detail
{
// Hot path. Inlined in the macro path: comparing one byte against another.
CRD_FORCEINLINE bool should_log(const Channel& ch, LogLevel level) noexcept
{
    return static_cast<u8>(level) >= static_cast<u8>(ch.runtime_level);
}

// The "slow path" of the macro: take an already-formatted message and
// route it to all sinks (sync) or push it onto the queue (async).
// 'message' may dangle after this returns; sinks that need long-term
// storage must copy it inside write().
void dispatch(LogLevel level, const Channel& ch, std::source_location loc, std::string_view message) noexcept;
} // namespace detail
} // namespace crd::log
