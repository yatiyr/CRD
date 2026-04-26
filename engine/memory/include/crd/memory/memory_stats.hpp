#pragma once

#include <crd/core/build_config.hpp>
#include <crd/core/types.hpp>

#include <atomic>

namespace crd::memory
{
// Per-allocator counters. Cheap to read, cheap to update.
//
// We keep the *fields* always present (so the public API never changes
// between Debug and Release) but only update them when CRD_DEBUG is set.
// Production builds pay zero overhead while still letting tools query
// a stats() block that's filled with zeros.
struct MemoryStats
{
    std::atomic<u64> alloc_count{0};
    std::atomic<u64> dealloc_count{0};
    std::atomic<u64> bytes_in_use{0}; // currently held by users
    std::atomic<u64> peak_bytes{0};   // highwater mark of bytes_in_use
    std::atomic<u64> total_bytes{0};  // lifetime cumulative

    // Plain-old-data snapshot for read-only consumers (tools, log dumps).
    struct Snapshot
    {
        u64 alloc_count;
        u64 dealloc_count;
        u64 bytes_in_use;
        u64 peak_bytes;
        u64 total_bytes;
    };

    Snapshot snapshot() const noexcept
    {
        Snapshot s;
        s.alloc_count = alloc_count.load(std::memory_order_relaxed);
        s.dealloc_count = dealloc_count.load(std::memory_order_relaxed);
        s.bytes_in_use = bytes_in_use.load(std::memory_order_relaxed);
        s.peak_bytes = peak_bytes.load(std::memory_order_relaxed);
        s.total_bytes = total_bytes.load(std::memory_order_relaxed);
        return s;
    }

    // ---- Mutators (called by allocator implementations) -------------
    // In release builds, these are no-ops so we don't pay for tracking.
    void on_allocate(u64 bytes) noexcept
    {
#if defined(CRD_DEBUG)
        alloc_count.fetch_add(1, std::memory_order_relaxed);
        total_bytes.fetch_add(bytes, std::memory_order_relaxed);
        const u64 in_use = bytes_in_use.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        // Race-tolerant peak update: it's fine if we lose a tiny update
        // under contention; this is a stat, not a correctness signal.
        u64 prev_peak = peak_bytes.load(std::memory_order_relaxed);
        while (in_use > prev_peak && !peak_bytes.compare_exchange_weak(prev_peak, in_use, std::memory_order_relaxed))
        {
            // retry
        }
#else
        (void)bytes;
#endif
    }

    void on_deallocate(u64 bytes) noexcept
    {
#if defined(CRD_DEBUG)
        dealloc_count.fetch_add(1, std::memory_order_relaxed);
        bytes_in_use.fetch_sub(bytes, std::memory_order_relaxed);
#else
        (void)bytes;
#endif
    }

    void reset() noexcept
    {
        alloc_count.store(0, std::memory_order_relaxed);
        dealloc_count.store(0, std::memory_order_relaxed);
        bytes_in_use.store(0, std::memory_order_relaxed);
        peak_bytes.store(0, std::memory_order_relaxed);
        total_bytes.store(0, std::memory_order_relaxed);
    }
};
} // namespace crd::memory
