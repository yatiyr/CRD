// test_offset_allocator.cpp — crd::memory::OffsetAllocator (ADR-0085 S6, Part A).
//
// The GPU-suballocation kernel, tested purely on the CPU (it manages offsets, not
// memory). Covers: basic allocate/free, alignment, fill-to-full + graceful failure,
// neighbour coalescing, free-then-reuse, the storage report, and a randomized
// alloc/free stress that asserts the core invariant — no two live allocations ever
// overlap — under ASan.

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/offset_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::memory::OffsetAllocator;

namespace
{
// Tiny deterministic LCG so the stress test is reproducible across runs/platforms.
class Lcg
{
public:
    explicit Lcg(crd::u64 seed) noexcept : m_state(seed) {}
    [[nodiscard]] crd::u32 next() noexcept
    {
        m_state = m_state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<crd::u32>(m_state >> 32);
    }
    [[nodiscard]] crd::u32 range(crd::u32 lo, crd::u32 hi) noexcept { return lo + (next() % (hi - lo + 1U)); }

private:
    crd::u64 m_state;
};
} // namespace

TEST_CASE("OffsetAllocator basic allocate/free and full-span report", "[memory][offset-alloc]")
{
    OffsetAllocator a(1024);
    REQUIRE(a.capacity() == 1024);
    REQUIRE(a.free_storage() == 1024);

    OffsetAllocator::Allocation x = a.allocate(256);
    REQUIRE(x.valid());
    REQUIRE(x.offset == 0); // first allocation lands at the span start
    REQUIRE(a.free_storage() == 1024 - 256);

    a.free(x);
    REQUIRE(a.free_storage() == 1024);
    const OffsetAllocator::StorageReport r = a.storage_report();
    REQUIRE(r.total_free_space == 1024);
    REQUIRE(r.largest_free_region == 1024); // coalesced back to one region
}

TEST_CASE("OffsetAllocator honors alignment", "[memory][offset-alloc]")
{
    OffsetAllocator a(64 * 1024);
    (void)a.allocate(13, 1); // shove the cursor to an unaligned position first
    for (crd::u32 align : {16U, 64U, 256U, 4096U})
    {
        OffsetAllocator::Allocation x = a.allocate(100, align);
        REQUIRE(x.valid());
        REQUIRE((x.offset % align) == 0);
    }
}

TEST_CASE("OffsetAllocator fills to capacity then fails gracefully", "[memory][offset-alloc]")
{
    OffsetAllocator a(4096);
    OffsetAllocator::Allocation whole = a.allocate(4096);
    REQUIRE(whole.valid());
    REQUIRE(a.free_storage() == 0);
    REQUIRE_FALSE(a.allocate(1).valid());     // nothing left
    REQUIRE_FALSE(a.allocate(8192).valid());  // larger than the whole span
    a.free(whole);
    REQUIRE(a.allocate(4096).valid());        // whole span available again
}

TEST_CASE("OffsetAllocator coalesces freed neighbours", "[memory][offset-alloc]")
{
    OffsetAllocator a(3072);
    OffsetAllocator::Allocation p0 = a.allocate(1024);
    OffsetAllocator::Allocation p1 = a.allocate(1024);
    OffsetAllocator::Allocation p2 = a.allocate(1024);
    REQUIRE(p0.valid());
    REQUIRE(p1.valid());
    REQUIRE(p2.valid());
    REQUIRE(a.free_storage() == 0);

    // Free in an order that exercises both prev- and next-neighbour coalescing.
    a.free(p1); // hole in the middle
    a.free(p0); // coalesces with p1's region (next neighbour)
    a.free(p2); // coalesces with the merged region (prev neighbour)

    const OffsetAllocator::StorageReport r = a.storage_report();
    REQUIRE(r.total_free_space == 3072);
    REQUIRE(r.largest_free_region == 3072); // fully merged -> one 3072 region
    REQUIRE(a.allocate(3072).valid());      // proves the merge produced one contiguous region
}

TEST_CASE("OffsetAllocator reuses a freed region", "[memory][offset-alloc]")
{
    OffsetAllocator a(2048);
    OffsetAllocator::Allocation x = a.allocate(512);
    const crd::u32              off = x.offset;
    a.free(x);
    OffsetAllocator::Allocation y = a.allocate(512);
    REQUIRE(y.valid());
    REQUIRE(y.offset == off); // freed region reused at the same offset
}

TEST_CASE("OffsetAllocator randomized stress keeps all live allocations disjoint", "[memory][offset-alloc]")
{
    constexpr crd::u32 k_capacity = 1U << 20; // 1 MiB span
    OffsetAllocator    a(k_capacity, 8192);

    struct Live
    {
        OffsetAllocator::Allocation alloc;
        crd::u32                    offset;
        crd::u32                    size;
    };
    crd::containers::Array<Live> live(crd::memory::default_allocator());

    Lcg rng(0xC0FFEEU);
    for (int iter = 0; iter < 20000; ++iter)
    {
        const bool do_alloc = (live.size() == 0) || ((rng.next() & 1U) != 0U);
        if (do_alloc)
        {
            const crd::u32              size = rng.range(1, 8192);
            OffsetAllocator::Allocation x    = a.allocate(size, 1); // alignment 1 -> offset is exact
            if (x.valid())
            {
                // Verify against every currently-live allocation: strictly disjoint.
                for (const Live& l : live)
                {
                    const bool disjoint = (x.offset + size <= l.offset) || (l.offset + l.size <= x.offset);
                    REQUIRE(disjoint);
                    REQUIRE(x.offset + size <= k_capacity); // in-bounds
                }
                live.push_back(Live{x, x.offset, size});
            }
        }
        else
        {
            const crd::u32 idx = rng.range(0, static_cast<crd::u32>(live.size()) - 1U);
            a.free(live[idx].alloc);
            live[idx] = live[live.size() - 1];
            live.pop_back();
        }
    }

    // Free the survivors; the span must coalesce fully back to one free region.
    for (const Live& l : live)
    {
        a.free(l.alloc);
    }
    const OffsetAllocator::StorageReport r = a.storage_report();
    REQUIRE(r.total_free_space == k_capacity);
    REQUIRE(r.largest_free_region == k_capacity);
}
