// Stress for the D-002 v4 primitives: AtomicArray<T> (bounded lock-free append)
// and the CacheLinePadded<u32> + std::atomic_ref "atomic element array" pattern.
//
//   - AtomicArray: N workers each append a fixed run of unique (worker, seq)
//     tokens into a shared AtomicArray sized exactly N*K. The oracle (between
//     rounds, quiescent) checks size() == N*K and every token present exactly
//     once — a reused slot index shows up as a missing token — then clear()s
//     for the next round.
//   - Padded atomic counters: N workers each do K atomic_ref fetch_add(1) on
//     random elements of a frozen Array<CacheLinePadded<u32>>; the oracle checks
//     the element sum == N*K, then zeroes them. (When you can use it,
//     jobs::parallel_reduce — local accumulate + serial fold — beats this; this
//     is the "must RMW shared cells" fallback, exercised here for correctness.)

#include "stress_harness.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/atomic_array.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

constexpr u32 kWorkers = 8U;
constexpr u32 kSeqBits = 40U;

constexpr u64 make_token(u32 worker, u64 seq) noexcept
{
    return (static_cast<u64>(worker) << kSeqBits) | seq;
}

void drive_append(crd::stress::RunMode mode)
{
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "aa-stress"};

    crd::stress::Config cfg = crd::stress::bounded(mode);
    cfg.num_workers = kWorkers;
    const u64 per_worker = cfg.iterations_per_round;
    const u64 total = per_worker * kWorkers;

    crd::containers::AtomicArray<u64> arr(static_cast<usize>(total), &alloc);
    crd::containers::Array<u8> seen(&alloc);
    seen.resize(static_cast<usize>(total), u8{0});
    crd::stress::FailSink sink;

    const auto work = [&arr, per_worker](u32 worker, u64 /*iterations*/, crd::stress::Rng& /*rng*/)
    {
        for (u64 seq = 0; seq < per_worker; ++seq)
        {
            const usize idx = arr.push(make_token(worker, seq));
            CRD_ASSERT(idx != crd::containers::AtomicArray<u64>::npos);
            (void)idx;
        }
    };

    const auto oracle = [&](u32 /*round*/)
    {
        if (arr.size() == 0U)
        {
            return; // just-cleared after the previous round (or before any work)
        }
        if (arr.size() != static_cast<usize>(total))
        {
            sink.fail("AtomicArray size != total pushed", arr.size(), 0U);
            return;
        }
        for (usize i = 0; i < seen.size(); ++i)
        {
            seen[i] = u8{0};
        }
        for (usize i = 0; i < arr.size(); ++i)
        {
            const u64 token = arr[i];
            const u32 worker = static_cast<u32>(token >> kSeqBits);
            const u64 seq = token & ((u64{1} << kSeqBits) - 1U);
            if (worker >= kWorkers || seq >= per_worker)
            {
                sink.fail("AtomicArray holds a malformed token", seq, worker);
                return;
            }
            ++seen[static_cast<usize>(worker) * static_cast<usize>(per_worker) + static_cast<usize>(seq)];
        }
        for (usize i = 0; i < seen.size(); ++i)
        {
            if (seen[i] != u8{1})
            {
                sink.fail("AtomicArray token present != once", static_cast<u64>(i), 0U);
                return;
            }
        }
        arr.clear(); // reset for the next round (quiescent point)
    };

    crd::stress::run(cfg, work, oracle);
    CRD_STRESS_ORACLE_OK(sink);
}

void drive_counters(crd::stress::RunMode mode)
{
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "aa-counters-stress"};

    crd::stress::Config cfg = crd::stress::bounded(mode);
    cfg.num_workers = kWorkers;
    const u64 per_worker = cfg.iterations_per_round; // fetch_adds per worker per round

    constexpr usize k_buckets = 257U; // odd; each CacheLinePadded slot is its own cache line
    crd::containers::Array<crd::containers::CacheLinePadded<u32>> buckets(k_buckets, &alloc);
    buckets.resize(k_buckets); // zero-initialised

    const u64 per_round = per_worker * kWorkers;
    crd::stress::FailSink sink;

    const auto work = [&buckets, per_worker](u32 /*worker*/, u64 /*iterations*/, crd::stress::Rng& rng)
    {
        for (u64 k = 0; k < per_worker; ++k)
        {
            const u32 b = rng.next_u32(static_cast<u32>(k_buckets));
            std::atomic_ref<u32>(buckets[b].value).fetch_add(1U, std::memory_order_relaxed);
        }
    };

    const auto oracle = [&](u32 /*round*/)
    {
        u64 sum = 0;
        for (usize i = 0; i < buckets.size(); ++i)
        {
            sum += buckets[i].value; // quiescent — direct read is fine
        }
        if (sum == 0U)
        {
            return; // just zeroed after the previous round (or before any work)
        }
        if (sum != per_round)
        {
            sink.fail("padded atomic-ref counter sum != workers * per_worker for this round", sum, 0U);
            return;
        }
        for (usize i = 0; i < buckets.size(); ++i)
        {
            buckets[i].value = 0U;
        }
    };

    {
        // `buckets` must not be structurally mutated during the parallel pass; element fetch_adds are fine.
        crd::containers::FrozenView<crd::containers::CacheLinePadded<u32>> guard(buckets);
        crd::stress::run(cfg, work, oracle);
    }
    CRD_STRESS_ORACLE_OK(sink);
}
} // namespace

TEST_CASE("AtomicArray stress -- concurrent append, every token once", "[stress][containers]")
{
    SECTION("threads")
    {
        drive_append(crd::stress::RunMode::Threads);
    }
    SECTION("fibers")
    {
        drive_append(crd::stress::RunMode::Fibers);
    }
}

TEST_CASE("AtomicArray stress -- padded atomic counter array, sum is exact", "[stress][containers]")
{
    SECTION("threads")
    {
        drive_counters(crd::stress::RunMode::Threads);
    }
    SECTION("fibers")
    {
        drive_counters(crd::stress::RunMode::Fibers);
    }
}

TEST_CASE("AtomicArray stress -- soak", "[stress][containers][.soak]")
{
    drive_append(crd::stress::RunMode::Threads);
    drive_counters(crd::stress::RunMode::Threads);
}
