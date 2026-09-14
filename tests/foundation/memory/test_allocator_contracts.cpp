// DIAG.3a/3d -- allocator contract edges: checked-arithmetic failure preserving the previous
// allocation, structural walkers, and wrong-owner / interior / double-free detection.
// Contract: docs/design/runtime-diagnostics.md#diag-3a, #diag-3d.

#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

namespace
{
namespace mem = crd::memory;
} // namespace

TEST_CASE("linear: an overflowing size fails like exhaustion and preserves the prior allocation",
          "[memory][contract][diag]")
{
    mem::LinearAllocator arena(1024U, nullptr, "diag-linear");

    void* first = arena.allocate(64U, 16U);
    REQUIRE(first != nullptr);

    // A near-SIZE_MAX request would wrap the offset small and slip past the capacity test if the
    // arithmetic were unchecked; checked_add makes it fail (nullptr), and the prior allocation is
    // untouched -- the next in-bounds allocation still succeeds.
    void* huge = arena.allocate(SIZE_MAX - 8U, 16U);
    CHECK(huge == nullptr);

    void* second = arena.allocate(64U, 16U);
    CHECK(second != nullptr);
    CHECK(second != first);
}

TEST_CASE("pool: structural walker accepts a healthy pool through alloc/dealloc", "[memory][contract][diag]")
{
    mem::PoolAllocator pool(32U, 8U, 16U, nullptr, "diag-pool");
    CHECK(pool.validate_structure());

    void* a = pool.allocate(32U);
    void* b = pool.allocate(32U);
    CHECK(pool.validate_structure());
    pool.deallocate(a);
    CHECK(pool.validate_structure());
    pool.deallocate(b);
    CHECK(pool.validate_structure());
    CHECK(pool.slots_in_use() == 0U);
}

TEST_CASE("pool: wrong-owner and interior pointers are distinguishable", "[memory][contract][diag]")
{
    mem::PoolAllocator a(32U, 4U, 16U, nullptr, "pool-a");
    mem::PoolAllocator b(32U, 4U, 16U, nullptr, "pool-b");

    void* pa = a.allocate(32U);
    REQUIRE(pa != nullptr);

    // Wrong-owner free: b never handed out pa.
    CHECK_FALSE(b.owns(pa));
    CHECK(a.owns(pa));

    // Interior pointer: inside a's buffer but not on a slot boundary. owns() (slot-strict for the
    // pool) and is_slot_aligned() both reject it, so an interior free is detectable.
    void* interior = static_cast<void*>(static_cast<crd::u8*>(pa) + 4);
    CHECK_FALSE(a.is_slot_aligned(interior));
    CHECK_FALSE(a.owns(interior));
    CHECK(a.is_slot_aligned(pa));

    a.deallocate(pa);
}

TEST_CASE("pool: the walker catches a double-free's corrupted free list", "[memory][contract][diag]")
{
    mem::PoolAllocator pool(32U, 4U, 16U, nullptr, "pool-df");

    void* p = pool.allocate(32U);
    REQUIRE(p != nullptr);
    pool.deallocate(p);
    CHECK(pool.validate_structure()); // one clean free

    // Freeing the same slot again makes the free list self-cyclic / over-length -- exactly what
    // the DIAG.3d walker exists to catch.
    pool.deallocate(p);
    CHECK_FALSE(pool.validate_structure());
}

TEST_CASE("tlsf: structural walker accepts a healthy heap across alloc/free/realloc", "[memory][contract][diag]")
{
    mem::TlsfAllocator heap(64U * 1024U, nullptr, "diag-tlsf");
    CHECK(heap.validate_structure());

    void* a = heap.try_allocate(100U, 16U);
    void* b = heap.try_allocate(2000U, 32U);
    void* c = heap.try_allocate(48U, 16U);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(heap.validate_structure());

    heap.deallocate(b); // frees a middle block -> coalesce with neighbours must stay consistent
    CHECK(heap.validate_structure());

    void* d = heap.reallocate(a, 100U, 1500U, 16U); // grow path (in-place or alloc-copy-free)
    REQUIRE(d != nullptr);
    CHECK(heap.validate_structure());

    heap.deallocate(c);
    heap.deallocate(d);
    CHECK(heap.validate_structure());
}

TEST_CASE("tlsf: randomized alloc/realloc/free sweep stays walker-clean and model-consistent",
          "[memory][contract][diag][stress]")
{
    // An independent allocation-state model (DIAG.3d): after every operation the structural walker
    // must pass, live ranges must never overlap, and each block's payload must still hold the byte
    // pattern written into it -- catching any metadata scribble into user memory or a corrupted chain.
    constexpr crd::usize kCapacity = 128U * 1024U;
    constexpr int        kMaxLive = 64;
    constexpr int        kIterations = 4000;

    mem::TlsfAllocator heap(kCapacity, nullptr, "tlsf-fuzz");

    struct Live
    {
        crd::u8*    p;
        crd::usize  size;
        crd::u8     pattern;
    };
    Live live[kMaxLive] = {};
    int  live_count = 0;

    crd::u64   rng = 0x9E3779B97F4A7C15ULL;
    auto       next = [&rng]() -> crd::u64 {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };

    auto overlaps = [&](const crd::u8* p, crd::usize size, int skip) -> bool {
        for (int i = 0; i < live_count; ++i)
        {
            if (i == skip)
            {
                continue;
            }
            const crd::u8* lo = live[i].p;
            const crd::u8* hi = lo + live[i].size;
            if (p < hi && lo < (p + size)) // half-open range intersection
            {
                return true;
            }
        }
        return false;
    };

    bool all_walker_ok = true;
    bool no_overlap = true;
    bool data_intact = true;

    for (int it = 0; it < kIterations; ++it)
    {
        const crd::u64 roll = next() % 100U;
        if (live_count < kMaxLive && (roll < 55U || live_count == 0)) // allocate
        {
            const crd::usize size = 8U + static_cast<crd::usize>(next() % 1024U);
            const crd::usize align = (next() & 1U) ? 16U : 32U;
            crd::u8* const  p = static_cast<crd::u8*>(heap.try_allocate(size, align));
            if (p != nullptr) // nullptr is a legal OOM outcome; the model just skips it
            {
                if (overlaps(p, size, -1))
                {
                    no_overlap = false;
                }
                const crd::u8 pat = static_cast<crd::u8>((it * 131 + 7) & 0xFF);
                std::memset(p, pat, size);
                live[live_count++] = Live{p, size, pat};
            }
        }
        else if (roll < 80U && live_count > 0) // reallocate a random live block
        {
            const int      idx = static_cast<int>(next() % static_cast<crd::u64>(live_count));
            const crd::usize old_size = live[idx].size;
            const crd::usize new_size = 8U + static_cast<crd::usize>(next() % 2048U);
            crd::u8* const  np = static_cast<crd::u8*>(heap.reallocate(live[idx].p, old_size, new_size, 16U));
            if (np != nullptr)
            {
                const crd::usize keep = old_size < new_size ? old_size : new_size;
                for (crd::usize k = 0; k < keep; ++k) // realloc must preserve the min-prefix bytes
                {
                    if (np[k] != live[idx].pattern)
                    {
                        data_intact = false;
                        break;
                    }
                }
                std::memset(np, live[idx].pattern, new_size); // re-stamp the (possibly grown) block
                live[idx].p = np;
                live[idx].size = new_size;
                if (overlaps(np, new_size, idx))
                {
                    no_overlap = false;
                }
            }
        }
        else if (live_count > 0) // free a random live block
        {
            const int idx = static_cast<int>(next() % static_cast<crd::u64>(live_count));
            for (crd::usize k = 0; k < live[idx].size; ++k) // payload survived until we free it
            {
                if (live[idx].p[k] != live[idx].pattern)
                {
                    data_intact = false;
                    break;
                }
            }
            heap.deallocate(live[idx].p);
            live[idx] = live[--live_count];
        }

        if (!heap.validate_structure())
        {
            all_walker_ok = false;
            break;
        }
    }

    CHECK(all_walker_ok);
    CHECK(no_overlap);
    CHECK(data_intact);

    for (int i = 0; i < live_count; ++i) // drain and confirm full recovery
    {
        heap.deallocate(live[i].p);
    }
    CHECK(heap.validate_structure());
}

TEST_CASE("tlsf: the walker catches seeded metadata corruption", "[memory][contract][diag]")
{
    // A caller-provided buffer lets the test stomp a known header field the way a heap overflow would,
    // and confirm the DIAG.3d walker reports a named failure instead of hanging or silently passing.
    alignas(16) static crd::u8 buffer[8192];
    mem::TlsfAllocator         heap(buffer, sizeof(buffer), "tlsf-corrupt");
    CHECK(heap.validate_structure());

    // Layout after init: [start sentinel @0 (16B)] [first free block @16] ...  The free block's
    // size_and_flags lives at offset 16 + 8 = 24. Inflate the size far past the region (keep the low
    // free bit) so the physical walk would step outside the pool -- exactly a corrupted size field.
    auto* const size_and_flags = reinterpret_cast<crd::u64*>(buffer + 24);
    *size_and_flags = static_cast<crd::u64>(sizeof(buffer) + 0x1000U) | 1U; // size huge, kFreeBit set

    CHECK_FALSE(heap.validate_structure());
}
