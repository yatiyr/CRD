#pragma once

#include <crd/core/types.hpp>
#include <crd/log/log_level.hpp>
#include <crd/log/log_sink.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace crd::log
{
// Stored copy of a record. The original LogRecord points into transient memory;
// we keep a self-contained struct so users can read it long after write() returned.
struct StoredLogRecord
{
    LogLevel level;
    std::string channel_name; // owned copy
    std::string message;      // owned copy
    std::string file;         // owned copy of source file
    u32 line;
    u64 thread_id;
    std::chrono::system_clock::time_point time;
};

// Keeps the most recent N records in memory.
// Future use case: an in-game console overlay (ImGui) calls snapshot() to render.
class RingBufferSink : public ISink
{
public:
    explicit RingBufferSink(usize capacity = 1024) noexcept;
    ~RingBufferSink() override = default;

    void write(const LogRecord& rec) override;
    void flush() override {}

    // Returns a copy of the records in chronological order (oldest first).
    std::vector<StoredLogRecord> snapshot() const;

    usize size() const noexcept;
    usize capacity() const noexcept { return m_capacity; }

    void clear() noexcept;

private:
    usize m_capacity;
    usize m_head = 0;  // index of next write
    usize m_count = 0; // current item count (<= capacity)
    std::vector<StoredLogRecord> m_buffer;
    mutable std::mutex m_mutex;
};
} // namespace crd::log
