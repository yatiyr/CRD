// Stress coverage for crd-containers under many concurrent fiber/thread workers
// (detour D-002, slice v0 — harness shakedown + the lock-free access patterns
// that hold *today*, before the v2 freeze()/FrozenView guard lands).
//
// Patterns exercised:
//   1. Disjoint parallel writes — N workers each own a contiguous slice of one
//      shared Array<u64>; each writes an idempotent per-(worker,index) value
//      every iteration. Race-free by construction (disjoint ranges), so the
//      oracle can verify every slot between rounds. A worker that scribbles
//      outside its slice corrupts a neighbour and the oracle (with the printed
//      seed) reproduces it. This is the pattern the v2 freeze() guard protects.
//   2. Many readers — N workers hammer random reads of one shared immutable
//      Array<u64> and fold the result into their own result slot. Mostly a
//      sanitiser exercise of the "lots of concurrent readers, zero sync" path.
//   3. Per-worker isolated TlsfAllocator + HashMap churn — each worker owns its
//      own allocator and map (allocators are single-threaded-by-contract), does
//      insert/erase/find churn against a shadow expectation, and checks size +
//      contents consistency. Proves the containers + allocator survive being
//      driven hard, concurrently, from many fibers — without being shared.

#include "stress_harness.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::usize;

constexpr u64 mix64(u64 x) noexcept
{
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return x;
}

// Idempotent value a given worker writes into a given index of its slice.
constexpr u64 slice_value(u32 worker, u32 local_index) noexcept
{
    return mix64((static_cast<u64>(worker) << 40) ^ static_cast<u64>(local_index) ^ 0xABCD1234ULL);
}

// Worker count for a bounded run: fiber mode uses the scheduler's worker count,
// thread mode a small fixed number — both pinned so shared-buffer sizing matches.
u32 bounded_workers(crd::stress::RunMode mode) noexcept
{
    if (mode == crd::stress::RunMode::Fibers)
    {
        const u32 n = crd::jobs::num_workers();
        return n > 0U ? n : 4U;
    }
    return 4U;
}
} // namespace

TEST_CASE("containers stress — disjoint parallel writes to a shared Array", "[stress][containers]")
{
    const auto body = [](crd::stress::RunMode mode)
    {
        crd::memory::MallocAllocator alloc("stress-disjoint");

        constexpr u32 per_worker = 257U; // odd — surfaces false-sharing perf bugs (not correctness)

        crd::stress::Config cfg = crd::stress::bounded(mode);
        const u32 workers = bounded_workers(mode);
        cfg.num_workers = workers;

        crd::containers::Array<u64> shared(static_cast<usize>(workers) * per_worker, &alloc);
        shared.resize(static_cast<usize>(workers) * per_worker, 0ULL);

        crd::stress::FailSink sink;

        const auto work = [&shared](u32 w, u64 iters, crd::stress::Rng& rng)
        {
            const usize base = static_cast<usize>(w) * per_worker;
            for (u64 it = 0; it < iters; ++it)
            {
                // Touch the slice in a scrambled order so an out-of-range worker
                // would collide on varied iterations, not always slot 0.
                const u32 j = rng.next_u32(per_worker);
                shared[base + j] = slice_value(w, j);
            }
            for (u32 j = 0; j < per_worker; ++j) // leave the slice in its canonical state
            {
                shared[base + j] = slice_value(w, j);
            }
        };

        const auto oracle = [&shared, workers, &sink](u32 /*round*/)
        {
            for (u32 w = 0; w < workers; ++w)
            {
                for (u32 j = 0; j < per_worker; ++j)
                {
                    if (shared[static_cast<usize>(w) * per_worker + j] != slice_value(w, j))
                    {
                        sink.fail("disjoint slot corrupted", j, w);
                        return;
                    }
                }
            }
        };

        crd::stress::run(cfg, work, oracle);
        CRD_STRESS_ORACLE_OK(sink);
    };

    SECTION("fibers")
    {
        body(crd::stress::RunMode::Fibers);
    }
    SECTION("threads")
    {
        body(crd::stress::RunMode::Threads);
    }
}

TEST_CASE("containers stress — many concurrent readers of an immutable Array", "[stress][containers]")
{
    const auto body = [](crd::stress::RunMode mode)
    {
        crd::memory::MallocAllocator alloc("stress-readers");

        constexpr usize n = 4096U;
        crd::containers::Array<u64> source(n, &alloc);
        for (usize i = 0; i < n; ++i)
        {
            source.push_back(mix64(i ^ 0x5EEDULL));
        }

        crd::stress::Config cfg = crd::stress::bounded(mode);
        const u32 workers = bounded_workers(mode);
        cfg.num_workers = workers;

        crd::containers::Array<u64> results(workers, &alloc);
        results.resize(workers, 0ULL);

        crd::stress::FailSink sink;

        const auto work = [&source, &results](u32 w, u64 iters, crd::stress::Rng& rng)
        {
            u64 acc = 0;
            for (u64 it = 0; it < iters; ++it)
            {
                acc ^= source[rng.next_u32(static_cast<u32>(n))];
            }
            results[w] = acc; // disjoint write into the worker's own result slot
        };

        const auto oracle = [&source, &sink](u32 /*round*/)
        {
            for (usize i = 0; i < n; ++i) // reads must not have mutated the source
            {
                if (source[i] != mix64(i ^ 0x5EEDULL))
                {
                    sink.fail("immutable source array mutated by readers", static_cast<u64>(i));
                    return;
                }
            }
        };

        crd::stress::run(cfg, work, oracle);
        CRD_STRESS_ORACLE_OK(sink);
    };

    SECTION("fibers")
    {
        body(crd::stress::RunMode::Fibers);
    }
    SECTION("threads")
    {
        body(crd::stress::RunMode::Threads);
    }
}

TEST_CASE("containers stress — per-worker isolated TlsfAllocator + HashMap churn", "[stress][containers]")
{
    const auto body = [](crd::stress::RunMode mode)
    {
        crd::stress::Config cfg = crd::stress::bounded(mode);
        crd::stress::FailSink sink;

        const auto work = [&sink](u32 w, u64 iters, crd::stress::Rng& rng)
        {
            // Each worker gets its own parent, heap and map — allocators are
            // single-threaded-by-contract, so isolation is the requirement.
            crd::memory::MallocAllocator parent("stress-worker-parent");
            crd::memory::TlsfAllocator heap(usize{1} << 20, &parent, "stress-worker-heap");
            crd::containers::HashMap<u64, u64> map(64U, &heap);

            constexpr u32 shadow_n = 64U;
            u64 keys[shadow_n] = {};
            u64 vals[shadow_n] = {};
            bool live[shadow_n] = {};
            u32 count = 0;

            for (u64 it = 0; it < iters; ++it)
            {
                const u32 slot = rng.next_u32(shadow_n);
                const u32 op = rng.next_u32(4U);

                if (op < 2U || !live[slot]) // insert/overwrite
                {
                    const u64 key =
                        (static_cast<u64>(w) << 48) ^ (static_cast<u64>(slot) << 8) ^ (rng.next_u64() & 0xFFULL);
                    const u64 val = rng.next_u64();
                    if (live[slot])
                    {
                        map.erase(keys[slot]);
                    }
                    else
                    {
                        ++count;
                    }
                    keys[slot] = key;
                    vals[slot] = val;
                    live[slot] = true;
                    map[key] = val;
                }
                else if (op == 2U) // erase
                {
                    map.erase(keys[slot]);
                    live[slot] = false;
                    --count;
                }
                else // find + verify
                {
                    const u64* found = map.find(keys[slot]);
                    CRD_STRESS_FAIL_IF(sink, w, it, found != nullptr && *found == vals[slot],
                                       "map lost a live entry or returned a stale value");
                }

                if ((it & 0x3FFULL) == 0ULL) // periodic full reconcile
                {
                    if (map.size() != count)
                    {
                        sink.fail("map size diverged from shadow count", it, w);
                        return;
                    }
                    for (u32 s = 0; s < shadow_n; ++s)
                    {
                        if (live[s])
                        {
                            const u64* f = map.find(keys[s]);
                            if (f == nullptr || *f != vals[s])
                            {
                                sink.fail("reconcile: live entry missing/stale", it, w);
                                return;
                            }
                        }
                    }
                }
            }
            for (u32 s = 0; s < shadow_n; ++s) // drain — exercises erase-to-empty + free path
            {
                if (live[s])
                {
                    map.erase(keys[s]);
                }
            }
            CRD_STRESS_FAIL_IF(sink, w, iters, map.size() == 0U, "map not empty after drain");
        };

        const auto oracle = [](u32 /*round*/) { /* nothing global — per-worker state only */ };

        crd::stress::run(cfg, work, oracle);
        CRD_STRESS_ORACLE_OK(sink);
    };

    SECTION("fibers")
    {
        body(crd::stress::RunMode::Fibers);
    }
    SECTION("threads")
    {
        body(crd::stress::RunMode::Threads);
    }
}
