// Stress + correctness for the D-002 v2 primitives: Array freeze guard /
// FrozenView, jobs::parallel_reduce.
//
//   - FrozenView + parallel_for: many fibers write disjoint slices of a frozen
//     Array; the freeze guard is active for exactly the parallel pass; every
//     element is verified between rounds. A worker that wrote outside its slice
//     corrupts a neighbour -> caught (FAIL prints the cell). A worker that
//     structurally mutated the array -> the freeze assert fires at the point of
//     misuse.
//   - parallel_reduce: result is checked against a serial reference across a
//     range of (count, num_jobs), including num_jobs > count.
//
// main_stress.cpp has already called jobs::init() for the binary.

#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

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

constexpr u64 cell_value(u32 round, usize index) noexcept
{
    return mix64((static_cast<u64>(round) << 40) ^ static_cast<u64>(index) ^ 0xF00DULL);
}

// One round: write the canonical per-cell value into a frozen `data` in
// parallel, then verify every cell on the calling thread.
void write_and_verify_round(crd::containers::Array<u64>& data, u32 num_jobs, u32 round)
{
    const u32 count = static_cast<u32>(data.size());
    {
        crd::containers::FrozenView<u64> fv(data); // frozen for exactly this scope
        crd::jobs::Counter* const c = crd::jobs::parallel_for(count, num_jobs,
                                                              [&fv, round](u32 begin, u32 end)
                                                              {
                                                                  for (u32 i = begin; i < end; ++i)
                                                                  {
                                                                      fv[i] = cell_value(round, i);
                                                                  }
                                                              });
        crd::jobs::wait(c);
    }
    CHECK_FALSE(data.is_frozen()); // unfrozen once the FrozenView left scope

    for (usize i = 0; i < data.size(); ++i)
    {
        if (data[i] != cell_value(round, i))
        {
            FAIL("frozen disjoint write: cell " << i << " wrong after round " << round << " (num_jobs=" << num_jobs
                                                << ")");
        }
    }
}
} // namespace

TEST_CASE("freeze stress -- FrozenView + parallel_for disjoint writes", "[stress][containers][jobs]")
{
    crd::memory::MallocAllocator alloc("freeze-stress");

    const u32 jobs_choices[] = {1U, 3U, 8U, 64U, 257U};
    for (u32 num_jobs : jobs_choices)
    {
        constexpr usize kN = 9001U; // odd, not a multiple of any job count
        crd::containers::Array<u64> data(kN, &alloc);
        data.resize(kN, 0ULL);
        for (u32 round = 0; round < 6U; ++round)
        {
            write_and_verify_round(data, num_jobs, round);
        }
    }

    crd::jobs::frame_reset(); // release the per-thread frame arenas this test consumed
}

TEST_CASE("freeze stress -- parallel_reduce matches a serial reference", "[stress][jobs]")
{
    const u32 counts[] = {1U, 2U, 7U, 64U, 1000U, 50'000U};
    const u32 jobs_choices[] = {1U, 2U, 8U, 64U, 1024U}; // includes num_jobs > count cases

    for (u32 count : counts)
    {
        u64 serial_sum = 0;
        u64 serial_max = 0;
        for (u32 i = 0; i < count; ++i)
        {
            const u64 v = mix64(i);
            serial_sum += v;
            if (v > serial_max)
            {
                serial_max = v;
            }
        }

        for (u32 num_jobs : jobs_choices)
        {
            const u64 par_sum = crd::jobs::parallel_reduce<u64>(
                count, num_jobs, 0ULL,
                [](u32 begin, u32 end)
                {
                    u64 s = 0;
                    for (u32 i = begin; i < end; ++i)
                    {
                        s += mix64(i);
                    }
                    return s;
                },
                [](u64 a, u64 b) { return a + b; });
            CHECK(par_sum == serial_sum);

            const u64 par_max = crd::jobs::parallel_reduce<u64>(
                count, num_jobs, 0ULL,
                [](u32 begin, u32 end)
                {
                    u64 m = 0;
                    for (u32 i = begin; i < end; ++i)
                    {
                        const u64 v = mix64(i);
                        if (v > m)
                        {
                            m = v;
                        }
                    }
                    return m;
                },
                [](u64 a, u64 b) { return a > b ? a : b; });
            CHECK(par_max == serial_max);
        }
    }

    crd::jobs::frame_reset();
}

TEST_CASE("freeze stress -- FrozenView + parallel_for long soak", "[stress][containers][jobs][.soak]")
{
    crd::memory::MallocAllocator alloc("freeze-soak");
    constexpr usize kN = 200'003U;
    crd::containers::Array<u64> data(kN, &alloc);
    data.resize(kN, 0ULL);

    for (u32 round = 0; round < 200U; ++round)
    {
        const u32 num_jobs = 1U + (round * 7U) % 96U;
        write_and_verify_round(data, num_jobs, round);
    }

    crd::jobs::frame_reset();
}
