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

    // Upper bound (milliseconds) on the flush the assert bridge performs before terminating: a headless build
    // must not hang forever draining a stuck sink on the way to the platform assert UI. flush_for() below uses
    // it; the ordinary flush() at shutdown is still unbounded. See DIAG.5c.
    u32 assert_flush_timeout_ms = 2000;
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

// Bounded flush for the assert / headless path: drain and flush, but never block longer than roughly
// `timeout_ms`. Returns true if the queue drained and every sink was flushed; false if it gave up because the
// queue was still backed up or a stuck sink held the sink lock (or it was called from the log worker itself). On
// a give-up it bumps flush_timeout_count() and writes one diagnostic line to stderr, so a stuck flush leaves
// evidence and terminates promptly instead of hanging. flush() is unchanged. See DIAG.5c.
bool flush_for(u32 timeout_ms) noexcept;

// Number of records dropped because the async queue was full.
u64 dropped_count() noexcept;

// Number of times flush_for() gave up before fully draining (stuck sink, queue pressure, or a reentrant call from
// inside a sink). A non-zero value is evidence a headless flush was bounded rather than allowed to hang.
u64 flush_timeout_count() noexcept;

// Number of asserts that fired from inside a sink and were suppressed (routing them back through the logger would
// re-lock the sink mutex on the owning thread and self-deadlock). Non-zero is evidence of sink-side assert
// reentrancy; the process still terminates via crd-core's own evidence + platform/default handler. See DIAG.5c(d).
u64 sink_reentrant_assert_count() noexcept;

// Number of times a sink's write()/flush() threw, violating the ISink "must not throw" contract. The throw is
// contained per-sink (counted, one stderr line, delivery continues to the other sinks) rather than terminating the
// noexcept worker. The logger does NOT auto-disable a failing sink -- removal stays the caller's decision via
// clear_sinks()/shutdown(). See DIAG.5c(e).
u64 sink_failure_count() noexcept;

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
