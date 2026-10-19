#pragma once

#include <crd/core/build_config.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/rt_sentinel.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/statistics_coverage.hpp>

#include <atomic>

// When an allocator's counters are updated:
//   - CRD_DEBUG                          -> always, from construction (the historical default; Debug and
//                                           RelWithDebInfo configurations)
//   - CRD_ENABLE_PROFILING=1 without it  -> once a reader requests them (`IAllocator::request_statistics()`, which
//                                           `crd::perf::register_allocator` calls), and only when the request comes
//                                           before the allocator's first allocation (DIAG.11b: counting every
//                                           allocator in the runtime-disabled mode cost +37.7% on allocation-heavy
//                                           frames; see docs/bench/2026-10-16-diag-11b-overhead.md)
//   - neither                            -> never: the statistics are compiled out and read as zeros
// CRD_MEMORY_STATS_TRACKING says whether this build compiles the counters in at all; CRD_MEMORY_STATS_ON_REQUEST says
// whether they wait for a request.
#if defined(CRD_DEBUG)
#define CRD_MEMORY_STATS_TRACKING 1   // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#define CRD_MEMORY_STATS_ON_REQUEST 0 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#elif CRD_ENABLE_PROFILING != 0
#define CRD_MEMORY_STATS_TRACKING 1   // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#define CRD_MEMORY_STATS_ON_REQUEST 1 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#else
#define CRD_MEMORY_STATS_TRACKING 0   // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#define CRD_MEMORY_STATS_ON_REQUEST 0 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#endif

namespace crd::memory
{
// Decides, per allocator, whether its operations are counted. Compiled into every build so its rules are tested on
// every lane (the race lanes included), although only a CRD_MEMORY_STATS_ON_REQUEST build consults it per operation.
//
// The invariant a free relies on: the gate reaches Counted only from Untouched, and Untouched means the allocator has
// never allocated. The first allocation leaves Untouched by its own exchange; if a request wins that race, the
// allocation sees Counted and counts itself. So a free that sees Counted frees a block that was counted, and every
// block allocated while the gate was not Counted is freed while it still is not (Uncounted and Refused never return
// to Untouched or reach Counted). Relaxed order is enough: the decision rests on one atomic's modification order, and a
// free happens after the allocation of its block.
class StatisticsGate
{
public:
    // The owning allocator counts its own operations. Called once from its constructor, before it allocates;
    // `always` starts it Counted (CRD_DEBUG), otherwise it waits for a request. An allocator that never calls this
    // keeps no counts of its own (a decorator, a composite or a test double) and reports StatisticsCoverage::Elsewhere.
    void keep_counts(bool always) noexcept { m_state.store(always ? kCounted : kUntouched, std::memory_order_relaxed); }

    // An allocation. True when it is counted, which means its free will be too. Inlined into every allocator: one
    // load and a branch; only an allocator's first allocation takes the exchange, out of line.
    [[nodiscard]] CRD_FORCEINLINE bool admit_allocation() noexcept
    {
        const u8 s = m_state.load(std::memory_order_relaxed);
        if (s == kCounted)
        {
            return true;
        }
        if (s == kUntouched)
        {
            return admit_first_allocation();
        }
        return false;
    }

    // A free (or an in-place resize) of a block this allocator handed out. True when the block was counted.
    [[nodiscard]] CRD_FORCEINLINE bool admit_release() const noexcept
    {
        return m_state.load(std::memory_order_relaxed) == kCounted;
    }

    // A reader asks for the counts. Complete when the allocator is (now) counted from its construction; TooLate when it
    // has already allocated without counting (it stays uncounted); Elsewhere for an allocator that keeps no counts.
    StatisticsCoverage request() noexcept
    {
        u8 s = m_state.load(std::memory_order_relaxed);
        for (;;)
        {
            if (s == kCounted)
            {
                return StatisticsCoverage::Complete;
            }
            if (s == kSilent)
            {
                return StatisticsCoverage::Elsewhere;
            }
            if (s == kRefused)
            {
                return StatisticsCoverage::TooLate;
            }
            const u8 next = (s == kUntouched) ? kCounted : kRefused;
            if (m_state.compare_exchange_weak(s, next, std::memory_order_relaxed))
            {
                return next == kCounted ? StatisticsCoverage::Complete : StatisticsCoverage::TooLate;
            }
        }
    }

    // What the counts cover now, without changing anything.
    [[nodiscard]] StatisticsCoverage coverage() const noexcept
    {
        switch (m_state.load(std::memory_order_relaxed))
        {
            case kCounted:
                return StatisticsCoverage::Complete;
            case kRefused:
                return StatisticsCoverage::TooLate;
            case kUntouched:
            case kUncounted:
                return StatisticsCoverage::NotRequested;
            default:
                return StatisticsCoverage::Elsewhere;
        }
    }

    // A move hands the source's blocks to this allocator, and with them the source's decision.
    void take(const StatisticsGate& other) noexcept
    {
        m_state.store(other.m_state.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

private:
    // The first allocation closes the door on a later request. Losing the exchange means a request claimed the
    // allocator first: this allocation is the first one it counts.
    CRD_NOINLINE bool admit_first_allocation() noexcept
    {
        u8 s = kUntouched;
        if (m_state.compare_exchange_strong(s, kUncounted, std::memory_order_relaxed))
        {
            return false;
        }
        return s == kCounted;
    }

    static constexpr u8 kSilent = 0U;    // keeps no counts of its own
    static constexpr u8 kUntouched = 1U; // counts on request; has not allocated
    static constexpr u8 kUncounted = 2U; // allocated before any request
    static constexpr u8 kRefused = 3U;   // a request came after the first allocation
    static constexpr u8 kCounted = 4U;

    std::atomic<u8> m_state{kSilent};
};

// Per-allocator counters. Cheap to read; what updating them costs depends on the build (above).
//
// The fields are present in every build, so the public API does not change between configurations; where the
// statistics are compiled out, or an allocator is not counted, they read as zeros and `coverage()` says why.
//
// Cost while counted: ~3 atomic fetch_add + a peak-bytes CAS loop per allocation, 2 fetch_add per free. Not counted
// (the runtime-disabled mode of a profiling build): one relaxed load and a branch per operation.
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

    // What the counters cover.
    [[nodiscard]] StatisticsCoverage coverage() const noexcept
    {
#if CRD_MEMORY_STATS_TRACKING
        return m_gate.coverage();
#else
        return StatisticsCoverage::CompiledOut;
#endif
    }

    // ---- Called by allocator implementations ---------------------------------------------------------------------

    // The allocator updates these counters itself (every concrete allocator that calls on_allocate). Call once from
    // its constructor, before its first allocation.
    void keep_counts() noexcept
    {
#if CRD_MEMORY_STATS_ON_REQUEST
        m_gate.keep_counts(false); // counted once a reader requests it
#else
        m_gate.keep_counts(true); // CRD_DEBUG counts from construction; a compiled-out build never asks the gate
#endif
    }

    // A reader's request (IAllocator::request_statistics).
    StatisticsCoverage request() noexcept
    {
#if CRD_MEMORY_STATS_TRACKING
        return m_gate.request();
#else
        return StatisticsCoverage::CompiledOut;
#endif
    }

    CRD_FORCEINLINE void on_allocate(u64 bytes) noexcept
    {
#if CRD_ENABLE_ASSERTS
        // Real-time sentinel: an allocation inside a declared RtScope is a forbidden operation -- report it
        // (then proceed; a sentinel never changes behaviour). Independent of stats tracking; zero-cost in
        // shipping builds. This is the shared allocation seam -- every concrete allocator calls on_allocate.
        if (crd::in_rt_scope())
        {
            crd::report_rt_violation(crd::RtViolationKind::Allocation, static_cast<crd::usize>(bytes));
        }
#endif
#if CRD_MEMORY_STATS_ON_REQUEST
        // The runtime-disabled mode's whole cost is the gate's inlined load and branch; the counting is a call
        // only when something asked for it.
        if (m_gate.admit_allocation())
        {
            count_allocation_out_of_line(bytes);
        }
#elif CRD_MEMORY_STATS_TRACKING
        count_allocation(bytes);
#else
        (void)bytes;
#endif
    }

    CRD_FORCEINLINE void on_deallocate(u64 bytes) noexcept
    {
#if CRD_MEMORY_STATS_ON_REQUEST
        if (m_gate.admit_release())
        {
            count_release_out_of_line(bytes);
        }
#elif CRD_MEMORY_STATS_TRACKING
        count_release(bytes);
#else
        (void)bytes;
#endif
    }

    // A live block resized in place: bytes_in_use (and the peak) follow its new extent; it is the same allocation, so
    // the allocation and deallocation counts and the real-time allocation sentinel are untouched.
    void on_resize(u64 old_bytes, u64 new_bytes) noexcept
    {
#if CRD_MEMORY_STATS_TRACKING
#if CRD_MEMORY_STATS_ON_REQUEST
        if (!m_gate.admit_release())
        {
            return;
        }
#endif
        if (new_bytes > old_bytes)
        {
            total_bytes.fetch_add(new_bytes - old_bytes, std::memory_order_relaxed);
            raise_in_use(new_bytes - old_bytes);
        }
        else
        {
            bytes_in_use.fetch_sub(old_bytes - new_bytes, std::memory_order_relaxed);
        }
#else
        (void)old_bytes;
        (void)new_bytes;
#endif
    }

    // A moving allocator hands its blocks, their counts and the counting decision to the destination; the source
    // keeps its decision and reads zero.
    void take(MemoryStats& other) noexcept
    {
        alloc_count.store(other.alloc_count.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
        dealloc_count.store(other.dealloc_count.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
        bytes_in_use.store(other.bytes_in_use.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
        peak_bytes.store(other.peak_bytes.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
        total_bytes.store(other.total_bytes.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
        m_gate.take(other.m_gate);
    }

    // Zeroes the counters; what they cover does not change.
    void reset() noexcept
    {
        alloc_count.store(0, std::memory_order_relaxed);
        dealloc_count.store(0, std::memory_order_relaxed);
        bytes_in_use.store(0, std::memory_order_relaxed);
        peak_bytes.store(0, std::memory_order_relaxed);
        total_bytes.store(0, std::memory_order_relaxed);
    }

private:
    void count_allocation(u64 bytes) noexcept
    {
        alloc_count.fetch_add(1, std::memory_order_relaxed);
        total_bytes.fetch_add(bytes, std::memory_order_relaxed);
        raise_in_use(bytes);
    }

    void count_release(u64 bytes) noexcept
    {
        dealloc_count.fetch_add(1, std::memory_order_relaxed);
        bytes_in_use.fetch_sub(bytes, std::memory_order_relaxed);
    }

    // Out of line so the unrequested path that every operation of a profiling build takes stays one load and a
    // branch at the call site.
    CRD_NOINLINE void count_allocation_out_of_line(u64 bytes) noexcept { count_allocation(bytes); }
    CRD_NOINLINE void count_release_out_of_line(u64 bytes) noexcept { count_release(bytes); }

    void raise_in_use(u64 bytes) noexcept
    {
        const u64 in_use = bytes_in_use.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        // Race-tolerant peak update: it's fine if we lose a tiny update
        // under contention; this is a stat, not a correctness signal.
        u64 prev_peak = peak_bytes.load(std::memory_order_relaxed);
        while (in_use > prev_peak && !peak_bytes.compare_exchange_weak(prev_peak, in_use, std::memory_order_relaxed))
        {
            // prev_peak reloaded on failure
        }
    }

    StatisticsGate m_gate;
};
} // namespace crd::memory
