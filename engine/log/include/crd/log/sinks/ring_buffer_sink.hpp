#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/log/log_level.hpp>
#include <crd/log/log_sink.hpp>
#include <crd/memory/allocator.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace crd::log
{
// Stored copy of a record. The original LogRecord points into transient
// memory; we keep a self-contained struct so users can read it long after
// write() returned.
//
// NOTE: we deliberately keep std::string here. crd-containers::String
// works too, but log records are populated from `std::source_location`
// and other std-typed sources, and a heap-backed std::string is exactly
// what we want for "owns its bytes, prints fine, no surprises".
struct StoredLogRecord
{
    LogLevel level;
    std::string channel_name;
    std::string message;
    std::string file;
    u32 line;
    u64 thread_id;
    std::chrono::system_clock::time_point time;
};

// Keeps the most recent N records in memory.
//
// Storage is `crd::containers::Array<StoredLogRecord>` (not std::vector
// any more — this is the v1d cleanup landing point) sized once at
// construction and used as a circular buffer with manual head/count
// indices. We keep the manual ring math because RingBufferSink's
// contract is "overwrite the oldest entry when full"; the bare
// `crd::containers::RingBuffer<T>` deliberately refuses on full instead.
//
// Future use case: an in-game console overlay (ImGui) calls snapshot()
// to render. The snapshot still returns std::vector for caller
// convenience (Array of an owning record type with an external API
// doesn't gain anything at the API boundary).
class RingBufferSink : public ISink
{
public:
    explicit RingBufferSink(usize capacity = 1024, memory::IAllocator* alloc = memory::default_allocator()) noexcept;
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
    usize m_head = 0;  // index of next write slot
    usize m_count = 0; // current item count (<= capacity)
    crd::containers::Array<StoredLogRecord> m_buffer;
    mutable std::mutex m_mutex;
};
} // namespace crd::log
