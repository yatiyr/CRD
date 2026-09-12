#pragma once

#include <crd/log/log_level.hpp>
#include <crd/log/log_record.hpp>

namespace crd::log
{
// Abstract base for "where logs end up".
// Implementations: ConsoleSink, FileSink, DebuggerSink, RingBufferSink, NullSink.
//
// Sinks are owned by the global Logger via std::unique_ptr<ISink>.
// The Logger guarantees: write() is never called concurrently from multiple
// threads in async mode (single worker), and is mutex-serialised in sync mode.
// Sinks therefore only need to be internally thread-safe if they expose
// public methods (e.g., RingBufferSink::snapshot()).
class ISink
{
public:
    virtual ~ISink() = default;

    // Write one record. Must not throw; on I/O failure, sinks should silently
    // drop or fall back. (Logging must never crash the program.)
    virtual void write(const LogRecord& rec) = 0;

    // Synchronously flush buffered output (file, stdio, etc.).
    virtual void flush() = 0;

    // Per-sink filter. The Logger has already done the channel filter; this
    // is a second screen so e.g., a FileSink can capture Trace while the
    // ConsoleSink only shows Info+.
    LogLevel min_level() const noexcept { return m_min_level; }
    void set_min_level(LogLevel l) noexcept { m_min_level = l; }

protected:
    LogLevel m_min_level = LogLevel::Trace;
};
} // namespace crd::log
