// DIAG.11b: what an allocator's statistics cover, and the gate a profiling build without CRD_DEBUG counts through.
//
// The gate is compiled into every build, so its rules are held on every lane, the race lanes included, although only
// win-shipping-profile (profiling without CRD_DEBUG) consults it per operation. The allocator cases assert each build's
// own answer: CRD_DEBUG counts every allocator from construction, a profiling build without it only after a request
// made before the first allocation, a build without either compiles the statistics out.

#include <crd/memory/allocators/growable_pool_allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/memory_stats.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <thread>
#include <utility>

using crd::usize;
using crd::memory::GrowablePoolAllocator;
using crd::memory::GrowableTlsfAllocator;
using crd::memory::StatisticsCoverage;
using crd::memory::StatisticsGate;
using crd::memory::ThreadSafeAllocator;
using crd::memory::TlsfAllocator;

namespace
{
constexpr usize kHeapBytes = usize{1} << 16U;

bool all_zero(const crd::memory::MemoryStats::Snapshot& s) noexcept
{
    return s.alloc_count == 0U && s.dealloc_count == 0U && s.bytes_in_use == 0U && s.peak_bytes == 0U &&
           s.total_bytes == 0U;
}
} // namespace

// ---- the gate's rules (every build) ---------------------------------------------------------------------------------

TEST_CASE("statistics gate: a request before the first allocation counts every operation", "[memory][stats][coverage]")
{
    StatisticsGate gate;
    gate.keep_counts(false);
    CHECK(gate.coverage() == StatisticsCoverage::NotRequested);
    CHECK(gate.request() == StatisticsCoverage::Complete);
    CHECK(gate.coverage() == StatisticsCoverage::Complete);
    CHECK(gate.admit_allocation());
    CHECK(gate.admit_release());
    CHECK(gate.request() == StatisticsCoverage::Complete); // asking again changes nothing
}

TEST_CASE("statistics gate: a request after the first allocation is refused and nothing is counted",
          "[memory][stats][coverage]")
{
    StatisticsGate gate;
    gate.keep_counts(false);
    CHECK_FALSE(gate.admit_allocation()); // the first allocation, unrequested
    CHECK(gate.coverage() == StatisticsCoverage::NotRequested);
    CHECK(gate.request() == StatisticsCoverage::TooLate);
    CHECK(gate.coverage() == StatisticsCoverage::TooLate);
    CHECK_FALSE(gate.admit_release());    // the first block's free stays uncounted, so nothing drifts
    CHECK_FALSE(gate.admit_allocation()); // and so does everything after the refusal
    CHECK(gate.request() == StatisticsCoverage::TooLate);
}

TEST_CASE("statistics gate: an allocator that keeps no counts answers Elsewhere", "[memory][stats][coverage]")
{
    StatisticsGate gate; // never told it keeps counts: a decorator, a composite or a test double
    CHECK(gate.coverage() == StatisticsCoverage::Elsewhere);
    CHECK(gate.request() == StatisticsCoverage::Elsewhere);
    CHECK_FALSE(gate.admit_allocation());
    CHECK_FALSE(gate.admit_release());
    CHECK(gate.coverage() == StatisticsCoverage::Elsewhere);
}

TEST_CASE("statistics gate: counting from construction needs no request", "[memory][stats][coverage]")
{
    StatisticsGate gate;
    gate.keep_counts(true); // what a CRD_DEBUG build's allocators do
    CHECK(gate.coverage() == StatisticsCoverage::Complete);
    CHECK(gate.admit_allocation());
    CHECK(gate.admit_release());
    CHECK(gate.request() == StatisticsCoverage::Complete);
}

TEST_CASE("statistics gate: a request racing the first allocations decides all of them alike",
          "[memory][stats][coverage][race]")
{
    // The free of a block is counted exactly when its allocation was, so the gate's one exchange must decide every
    // racing allocation the same way as the request: all counted when the request won, none when an allocation won.
    constexpr int rounds = 300;
    constexpr int allocators = 3;
    constexpr int ops_per_thread = 8;
    int requested_first = 0;
    int allocated_first = 0;
    int inconsistent = 0;
    for (int round = 0; round < rounds; ++round)
    {
        StatisticsGate gate;
        gate.keep_counts(false);
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::atomic<int> admitted{0};
        std::atomic<int> refused{0};
        StatisticsCoverage answer = StatisticsCoverage::NotRequested;
        std::thread threads[allocators + 1]; // NOLINT(modernize-avoid-c-arrays): a fixed set per round
        for (int t = 0; t < allocators; ++t)
        {
            threads[t] = std::thread(
                [&]()
                {
                    ready.fetch_add(1, std::memory_order_acq_rel);
                    while (!go.load(std::memory_order_acquire))
                    {
                        std::this_thread::yield();
                    }
                    for (int op = 0; op < ops_per_thread; ++op)
                    {
                        if (gate.admit_allocation())
                        {
                            admitted.fetch_add(1, std::memory_order_relaxed);
                        }
                        else
                        {
                            refused.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                });
        }
        threads[allocators] = std::thread(
            [&]()
            {
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (!go.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                answer = gate.request();
            });
        while (ready.load(std::memory_order_acquire) != allocators + 1)
        {
            std::this_thread::yield();
        }
        go.store(true, std::memory_order_release);
        for (std::thread& t : threads)
        {
            t.join();
        }
        const int total = allocators * ops_per_thread;
        const bool counted = answer == StatisticsCoverage::Complete;
        if (counted)
        {
            ++requested_first;
        }
        else
        {
            ++allocated_first;
        }
        const bool alike = counted ? (admitted.load() == total && refused.load() == 0 && gate.admit_release())
                                   : (answer == StatisticsCoverage::TooLate && admitted.load() == 0 &&
                                      refused.load() == total && !gate.admit_release());
        if (!alike)
        {
            ++inconsistent;
        }
    }
    INFO("rounds the request won: " << requested_first << ", rounds an allocation won: " << allocated_first);
    CHECK(inconsistent == 0);
    CHECK(requested_first + allocated_first == rounds);
}

// ---- real allocators: each build's own answer
// ------------------------------------------------------------------------

TEST_CASE("statistics coverage: an allocator requested before its first allocation is counted completely",
          "[memory][stats][coverage]")
{
    TlsfAllocator heap(kHeapBytes, nullptr, "coverage-early");
    const StatisticsCoverage answer = heap.request_statistics();
    void* const p = heap.allocate(100U);
    void* const q = heap.allocate(200U);
    const auto mid = heap.stats().snapshot();
    heap.deallocate(p);
    heap.deallocate(q);
    const auto after = heap.stats().snapshot();
#if CRD_MEMORY_STATS_TRACKING
    CHECK(answer == StatisticsCoverage::Complete);
    CHECK(heap.stats().coverage() == StatisticsCoverage::Complete);
    CHECK(mid.alloc_count == 2U);
    CHECK(mid.bytes_in_use >= 300U);
    CHECK(after.dealloc_count == 2U);
    CHECK(after.bytes_in_use == 0U);
    CHECK(after.peak_bytes >= 300U);
    CHECK(after.total_bytes >= 300U);
#else
    CHECK(answer == StatisticsCoverage::CompiledOut);
    CHECK(heap.stats().coverage() == StatisticsCoverage::CompiledOut);
    CHECK(all_zero(mid));
    CHECK(all_zero(after));
#endif
}

TEST_CASE("statistics coverage: a request after the first allocation", "[memory][stats][coverage]")
{
    TlsfAllocator heap(kHeapBytes, nullptr, "coverage-late");
    void* const p = heap.allocate(100U);
    const StatisticsCoverage answer = heap.request_statistics();
    void* const q = heap.allocate(200U);
    heap.deallocate(p); // a block from before the request
    heap.deallocate(q);
    const auto after = heap.stats().snapshot();
#if CRD_MEMORY_STATS_ON_REQUEST
    // Counting from here would subtract the first block's free from counts that never held it.
    CHECK(answer == StatisticsCoverage::TooLate);
    CHECK(heap.stats().coverage() == StatisticsCoverage::TooLate);
    CHECK(all_zero(after));
#elif CRD_MEMORY_STATS_TRACKING
    CHECK(answer == StatisticsCoverage::Complete); // counted from construction
    CHECK(after.alloc_count == 2U);
    CHECK(after.dealloc_count == 2U);
    CHECK(after.bytes_in_use == 0U);
#else
    CHECK(answer == StatisticsCoverage::CompiledOut);
    CHECK(all_zero(after));
#endif
}

TEST_CASE("statistics coverage: an allocator nothing asks about", "[memory][stats][coverage]")
{
    TlsfAllocator heap(kHeapBytes, nullptr, "coverage-unasked");
    for (int i = 0; i < 16; ++i)
    {
        heap.deallocate(heap.allocate(64U + static_cast<usize>(i)));
    }
    const auto s = heap.stats().snapshot();
#if CRD_MEMORY_STATS_ON_REQUEST
    // The runtime-disabled mode: the operations ran and nothing was counted.
    CHECK(heap.stats().coverage() == StatisticsCoverage::NotRequested);
    CHECK(all_zero(s));
#elif CRD_MEMORY_STATS_TRACKING
    CHECK(heap.stats().coverage() == StatisticsCoverage::Complete);
    CHECK(s.alloc_count == 16U);
    CHECK(s.dealloc_count == 16U);
    CHECK(s.bytes_in_use == 0U);
#else
    CHECK(heap.stats().coverage() == StatisticsCoverage::CompiledOut);
    CHECK(all_zero(s));
#endif
}

TEST_CASE("statistics coverage: decorators and composites keep no counts of their own", "[memory][stats][coverage]")
{
    TlsfAllocator inner(kHeapBytes, nullptr, "coverage-inner");
    ThreadSafeAllocator locked(&inner, "coverage-locked");
    GrowableTlsfAllocator grown(kHeapBytes, nullptr, "coverage-grown");
#if CRD_MEMORY_STATS_TRACKING
    CHECK(locked.request_statistics() == StatisticsCoverage::Elsewhere);
    CHECK(grown.request_statistics() == StatisticsCoverage::Elsewhere);
    CHECK(inner.request_statistics() == StatisticsCoverage::Complete); // before its first allocation
    locked.deallocate(locked.allocate(64U));
    grown.deallocate(grown.allocate(64U));
    CHECK(inner.stats().snapshot().alloc_count == 1U);
    CHECK(inner.stats().snapshot().dealloc_count == 1U);
    CHECK(all_zero(locked.stats().snapshot()));
    CHECK(all_zero(grown.stats().snapshot()));
    CHECK(locked.stats().coverage() == StatisticsCoverage::Elsewhere);
#else
    CHECK(locked.request_statistics() == StatisticsCoverage::CompiledOut);
    CHECK(grown.request_statistics() == StatisticsCoverage::CompiledOut);
    CHECK(inner.request_statistics() == StatisticsCoverage::CompiledOut);
#endif
}

TEST_CASE("statistics coverage: a moved growable pool hands its counts to the destination", "[memory][stats][coverage]")
{
    GrowablePoolAllocator source(32U, 16U, 8U, nullptr, "coverage-move");
    const StatisticsCoverage answer = source.request_statistics();
    void* const a = source.allocate(32U, 16U);
    void* const b = source.allocate(32U, 16U);
    GrowablePoolAllocator moved(std::move(source));
    moved.deallocate(a);
    moved.deallocate(b);
    const auto s = moved.stats().snapshot();
#if CRD_MEMORY_STATS_TRACKING
    CHECK(answer == StatisticsCoverage::Complete);
    CHECK(moved.stats().coverage() == StatisticsCoverage::Complete);
    CHECK(s.alloc_count == 2U);
    CHECK(s.dealloc_count == 2U);
    CHECK(s.bytes_in_use == 0U);                // the destination freed blocks it was handed, never below zero
    CHECK(all_zero(source.stats().snapshot())); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
#else
    CHECK(answer == StatisticsCoverage::CompiledOut);
    CHECK(all_zero(s));
#endif
}

TEST_CASE("statistics coverage: every value has a stable name", "[memory][stats][coverage]")
{
    CHECK(std::string_view{crd::memory::statistics_coverage_name(StatisticsCoverage::Complete)} == "complete");
    CHECK(std::string_view{crd::memory::statistics_coverage_name(StatisticsCoverage::NotRequested)} == "not_requested");
    CHECK(std::string_view{crd::memory::statistics_coverage_name(StatisticsCoverage::TooLate)} == "too_late");
    CHECK(std::string_view{crd::memory::statistics_coverage_name(StatisticsCoverage::Elsewhere)} == "elsewhere");
    CHECK(std::string_view{crd::memory::statistics_coverage_name(StatisticsCoverage::CompiledOut)} == "compiled_out");
    CHECK(std::string_view{crd::memory::statistics_coverage_name(StatisticsCoverage::Unrecorded)} == "unrecorded");
}
