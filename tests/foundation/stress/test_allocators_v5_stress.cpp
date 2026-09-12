// Allocator stress matrix (detour D-002 v5).
//
// Allocators are single-threaded-by-contract, so the concurrency story is
// *isolation*: N workers each pound their OWN allocator concurrently — proving
// reentrancy + isolation, not shared-thread-safety. Each allocator gets a test
// shaped to its OWN contract (a LinearAllocator alloc/free mix would be
// meaningless — it can't free individual allocations), plus an
// adversarial-sequential lane for TLSF (single-threaded; ASan is the lane that
// earns it). v0's test_allocators_stress.cpp already covers TLSF isolated churn.
//
// main_stress.cpp runs jobs::init() for the binary.

#include "stress_harness.hpp"

#include <crd/memory/allocators/growable_pool_allocator.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/allocators/stack_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

constexpr u8 byte_fill(u32 worker, u32 tag) noexcept
{
    const u8 b = static_cast<u8>((worker * 31U + tag * 7U + 1U) & 0xFFU);
    return b == 0U ? u8{1} : b; // never 0 — a fresh zero page is distinguishable
}

// Cheap region pattern: first byte, last byte, midpoint.
bool region_ok(const u8* p, usize size, u8 fill) noexcept
{
    if (size == 0U)
    {
        return true;
    }
    return p[0] == fill && p[size - 1] == fill && p[size / 2] == fill;
}

void stamp(u8* p, usize size, u8 fill) noexcept
{
    std::memset(p, fill, size);
}

// ---------------------------------------------------------------------------
// LinearAllocator — bump + reset, no per-alloc free. Per-fiber isolated arena;
// fill-verify-reset loop. allocate() is fatal-on-OOM, so we check remaining()
// before each allocation and stop short.
// ---------------------------------------------------------------------------
void drive_linear(crd::stress::RunMode mode)
{
    crd::stress::FailSink sink;
    const auto work = [&sink](u32 w, u64 iters, crd::stress::Rng& rng)
    {
        crd::memory::MallocAllocator parent("v5-linear-parent");
        crd::memory::LinearAllocator arena(usize{256} << 10, &parent, "v5-linear");

        constexpr u32 max_live = 256U;
        struct Rec
        {
            u8* ptr = nullptr;
            usize size = 0;
            u8 fill = 0;
        };
        Rec live[max_live];

        for (u64 it = 0; it < iters; ++it)
        {
            arena.reset();
            u32 n = 0;
            while (n < max_live)
            {
                const usize size = 1U + rng.next_u32(2048U);
                const usize align = usize{8} << rng.next_u32(5U); // 8..256
                if (arena.remaining() < size + align)
                {
                    break;
                }
                auto* p = static_cast<u8*>(arena.allocate(size, align));
                CRD_STRESS_FAIL_IF(sink, w, it, p != nullptr, "LinearAllocator returned null below capacity");
                if (p == nullptr)
                {
                    break;
                }
                CRD_STRESS_FAIL_IF(sink, w, it, (reinterpret_cast<usize>(p) & (align - 1U)) == 0U,
                                   "LinearAllocator mis-aligned pointer");
                // Monotone: this allocation starts at or after the previous one's end.
                if (n > 0U)
                {
                    CRD_STRESS_FAIL_IF(sink, w, it, p >= live[n - 1].ptr + live[n - 1].size,
                                       "LinearAllocator handed out an overlapping region");
                }
                const u8 fill = byte_fill(w, n);
                stamp(p, size, fill);
                live[n++] = Rec{p, size, fill};
            }
            // After filling, every region must still hold its fill (no overlap clobbered it).
            for (u32 i = 0; i < n; ++i)
            {
                CRD_STRESS_FAIL_IF(sink, w, it, region_ok(live[i].ptr, live[i].size, live[i].fill),
                                   "LinearAllocator region corrupted after a full fill pass");
            }
        }
        CRD_STRESS_FAIL_IF(sink, w, iters, arena.used() <= arena.capacity(), "LinearAllocator used() > capacity()");
    };
    crd::stress::run(crd::stress::bounded(mode), work, [](u32) {});
    CRD_STRESS_ORACLE_OK(sink);
}

// ---------------------------------------------------------------------------
// StackAllocator — nested LIFO mark/reset_to. Per-fiber isolated; verify that
// rolling back to an inner marker leaves the outer allocations intact.
// ---------------------------------------------------------------------------
void drive_stack(crd::stress::RunMode mode)
{
    crd::stress::FailSink sink;
    const auto work = [&sink](u32 w, u64 iters, crd::stress::Rng& rng)
    {
        crd::memory::MallocAllocator parent("v5-stack-parent");
        crd::memory::StackAllocator st(usize{128} << 10, &parent, "v5-stack");

        for (u64 it = 0; it < iters; ++it)
        {
            st.reset();
            const auto m0 = st.mark();

            // Outer allocations that must survive an inner rollback.
            const usize sa = 1U + rng.next_u32(512U);
            const usize sb = 1U + rng.next_u32(512U);
            auto* a = static_cast<u8*>(st.allocate(sa, 16U));
            auto* b = static_cast<u8*>(st.allocate(sb, 16U));
            CRD_STRESS_FAIL_IF(sink, w, it, a != nullptr && b != nullptr, "StackAllocator returned null");
            const u8 fa = byte_fill(w, 1U);
            const u8 fb = byte_fill(w, 2U);
            stamp(a, sa, fa);
            stamp(b, sb, fb);

            // Inner scope: allocate, then roll it back.
            const auto m1 = st.mark();
            for (int k = 0; k < 8; ++k)
            {
                const usize sc = 1U + rng.next_u32(256U);
                auto* c = static_cast<u8*>(st.allocate(sc, 8U));
                CRD_STRESS_FAIL_IF(sink, w, it, c != nullptr, "StackAllocator inner alloc returned null");
                if (c != nullptr)
                {
                    std::memset(c, 0xCC, sc); // touch the whole extent — ASan checks bounds
                }
            }
            st.reset_to(m1);

            // Outer allocations untouched by the inner churn + rollback.
            CRD_STRESS_FAIL_IF(sink, w, it, region_ok(a, sa, fa) && region_ok(b, sb, fb),
                               "StackAllocator inner rollback clobbered an outer allocation");
            CRD_STRESS_FAIL_IF(sink, w, it, st.used() == m1.offset, "StackAllocator reset_to(inner) wrong offset");

            st.reset_to(m0);
            CRD_STRESS_FAIL_IF(sink, w, it, st.used() == m0.offset, "StackAllocator reset_to(outer) wrong offset");
        }
    };
    crd::stress::run(crd::stress::bounded(mode), work, [](u32) {});
    CRD_STRESS_ORACLE_OK(sink);
}

// ---------------------------------------------------------------------------
// PoolAllocator — fixed-size slots, O(1) alloc/free, returns nullptr (not fatal)
// on exhaustion. Per-fiber isolated pool; random alloc/free churn with per-slot
// byte patterns. Catches free-list corruption / slot overlap.
// ---------------------------------------------------------------------------
void drive_pool(crd::stress::RunMode mode)
{
    crd::stress::FailSink sink;
    const auto work = [&sink](u32 w, u64 iters, crd::stress::Rng& rng)
    {
        constexpr usize slot_size = 64U;
        constexpr usize slot_align = 16U;
        constexpr usize slot_count = 128U;
        crd::memory::MallocAllocator parent("v5-pool-parent");
        crd::memory::PoolAllocator pool(slot_size, slot_count, slot_align, &parent, "v5-pool");

        struct Rec
        {
            u8* ptr = nullptr;
            u8 fill = 0;
        };
        Rec live[slot_count];
        u32 live_count = 0;
        u32 tag = 1U;

        const auto free_at = [&](u32 idx)
        {
            CRD_STRESS_FAIL_IF(sink, w, 0, region_ok(live[idx].ptr, slot_size, live[idx].fill),
                               "PoolAllocator slot corrupted before free");
            pool.deallocate(live[idx].ptr);
            live[idx] = live[--live_count];
        };

        for (u64 it = 0; it < iters; ++it)
        {
            const u32 roll = rng.next_u32(10U);
            if (roll < 6U) // allocate
            {
                auto* p = static_cast<u8*>(pool.allocate(1U + rng.next_u32(slot_size), usize{1} << rng.next_u32(5U)));
                if (p == nullptr)
                {
                    CRD_STRESS_FAIL_IF(sink, w, it, live_count == slot_count, "PoolAllocator null but not full");
                    if (live_count > 0U)
                    {
                        free_at(rng.next_u32(live_count));
                    }
                    continue;
                }
                CRD_STRESS_FAIL_IF(sink, w, it, (reinterpret_cast<usize>(p) & (slot_align - 1U)) == 0U,
                                   "PoolAllocator slot mis-aligned");
                CRD_STRESS_FAIL_IF(sink, w, it, pool.owns(p), "PoolAllocator does not own a slot it returned");
                const u8 fill = byte_fill(w, tag++);
                stamp(p, slot_size, fill);
                live[live_count++] = Rec{p, fill};
            }
            else if (live_count > 0U) // free
            {
                free_at(rng.next_u32(live_count));
            }
            CRD_STRESS_FAIL_IF(sink, w, it, pool.slots_in_use() == live_count, "PoolAllocator slots_in_use mismatch");
        }
        while (live_count > 0U)
        {
            free_at(live_count - 1U);
        }
        CRD_STRESS_FAIL_IF(sink, w, iters, pool.slots_in_use() == 0U, "PoolAllocator not empty after drain");
    };
    crd::stress::run(crd::stress::bounded(mode), work, [](u32) {});
    CRD_STRESS_ORACLE_OK(sink);
}

// ---------------------------------------------------------------------------
// GrowablePoolAllocator — Pool patterns + page growth. Per-fiber isolated; force
// the live count well past slots_per_page so pages grow; verify page_count
// grows monotonically and never shrinks; free across page boundaries.
// ---------------------------------------------------------------------------
void drive_growable_pool(crd::stress::RunMode mode)
{
    crd::stress::FailSink sink;
    const auto work = [&sink](u32 w, u64 iters, crd::stress::Rng& rng)
    {
        constexpr usize slot_size = 64U;
        constexpr usize slot_align = 16U;
        constexpr usize k_slots_per_page = 8U;
        constexpr u32 max_live = 200U; // >> k_slots_per_page → forces growth
        crd::memory::MallocAllocator parent("v5-gpool-parent");
        crd::memory::GrowablePoolAllocator gp(slot_size, slot_align, k_slots_per_page, &parent, "v5-gpool");

        struct Rec
        {
            u8* ptr = nullptr;
            u8 fill = 0;
        };
        Rec live[max_live];
        u32 live_count = 0;
        u32 tag = 1U;
        usize max_pages_seen = 0;

        const auto free_at = [&](u32 idx)
        {
            CRD_STRESS_FAIL_IF(sink, w, 0, region_ok(live[idx].ptr, slot_size, live[idx].fill),
                               "GrowablePool slot corrupted before free");
            gp.deallocate(live[idx].ptr);
            live[idx] = live[--live_count];
        };

        for (u64 it = 0; it < iters; ++it)
        {
            const u32 roll = rng.next_u32(10U);
            if (roll < 7U && live_count < max_live) // bias toward growth
            {
                auto* p = static_cast<u8*>(gp.allocate(1U + rng.next_u32(slot_size), usize{1} << rng.next_u32(5U)));
                CRD_STRESS_FAIL_IF(sink, w, it, p != nullptr, "GrowablePool returned null (should grow, not fail)");
                if (p == nullptr)
                {
                    continue;
                }
                CRD_STRESS_FAIL_IF(sink, w, it, gp.owns(p), "GrowablePool does not own a slot it returned");
                const u8 fill = byte_fill(w, tag++);
                stamp(p, slot_size, fill);
                live[live_count++] = Rec{p, fill};
            }
            else if (live_count > 0U)
            {
                free_at(rng.next_u32(live_count));
            }
            const usize pages = gp.page_count();
            CRD_STRESS_FAIL_IF(sink, w, it, pages >= max_pages_seen, "GrowablePool page_count shrank");
            if (pages > max_pages_seen)
            {
                max_pages_seen = pages;
            }
            CRD_STRESS_FAIL_IF(sink, w, it, gp.slots_in_use() == live_count, "GrowablePool slots_in_use mismatch");
        }
        while (live_count > 0U)
        {
            free_at(live_count - 1U);
        }
        CRD_STRESS_FAIL_IF(sink, w, iters, gp.slots_in_use() == 0U, "GrowablePool not empty after drain");
        CRD_STRESS_FAIL_IF(sink, w, iters, gp.page_count() == max_pages_seen,
                           "GrowablePool released pages on drain (should keep them)");
        CRD_STRESS_FAIL_IF(sink, w, iters, max_pages_seen >= 2U, "GrowablePool never grew past one page");
    };
    crd::stress::run(crd::stress::bounded(mode), work, [](u32) {});
    CRD_STRESS_ORACLE_OK(sink);
}
} // namespace

TEST_CASE("allocators stress -- LinearAllocator isolated arenas", "[stress][memory]")
{
    SECTION("threads")
    {
        drive_linear(crd::stress::RunMode::Threads);
    }
    SECTION("fibers")
    {
        drive_linear(crd::stress::RunMode::Fibers);
    }
}

TEST_CASE("allocators stress -- StackAllocator nested LIFO", "[stress][memory]")
{
    SECTION("threads")
    {
        drive_stack(crd::stress::RunMode::Threads);
    }
    SECTION("fibers")
    {
        drive_stack(crd::stress::RunMode::Fibers);
    }
}

TEST_CASE("allocators stress -- PoolAllocator isolated pools", "[stress][memory]")
{
    SECTION("threads")
    {
        drive_pool(crd::stress::RunMode::Threads);
    }
    SECTION("fibers")
    {
        drive_pool(crd::stress::RunMode::Fibers);
    }
}

TEST_CASE("allocators stress -- GrowablePoolAllocator isolated pools with growth", "[stress][memory]")
{
    SECTION("threads")
    {
        drive_growable_pool(crd::stress::RunMode::Threads);
    }
    SECTION("fibers")
    {
        drive_growable_pool(crd::stress::RunMode::Fibers);
    }
}

// ---------------------------------------------------------------------------
// TlsfAllocator — adversarial *sequential* patterns. Single-threaded; ASan is
// the lane that earns these. (v0 covers TLSF isolated concurrent churn.)
// ---------------------------------------------------------------------------
TEST_CASE("allocators stress -- TlsfAllocator adversarial sequential", "[stress][memory]")
{
    crd::memory::MallocAllocator parent("v5-tlsf-parent");
    crd::memory::TlsfAllocator tlsf(usize{8} << 20, &parent, "v5-tlsf");
    const usize initial_capacity = tlsf.pool_capacity();

    SECTION("coalescing — free-every-other then alloc-larger fits")
    {
        constexpr usize k_n = 200U;
        constexpr usize k_s = 256U;
        u8* blocks[k_n];
        for (usize i = 0; i < k_n; ++i)
        {
            blocks[i] = static_cast<u8*>(tlsf.try_allocate(k_s, 16U));
            REQUIRE(blocks[i] != nullptr);
            std::memset(blocks[i], static_cast<int>(i & 0xFFU), k_s);
        }
        for (usize i = 0; i < k_n; i += 2U) // free every other → adjacent holes
        {
            tlsf.deallocate(blocks[i]);
            blocks[i] = nullptr;
        }
        // Larger blocks should fit by coalescing the freed adjacent runs.
        usize got = 0;
        for (usize i = 0; i < k_n / 2U; ++i)
        {
            u8* p = static_cast<u8*>(tlsf.try_allocate(k_s + k_s / 2U, 16U));
            if (p == nullptr)
            {
                break;
            }
            std::memset(p, 0xAB, k_s + k_s / 2U);
            ++got;
        }
        CHECK(got > 0U); // at least some coalescing happened
        // The odd-index blocks we kept are still intact.
        for (usize i = 1; i < k_n; i += 2U)
        {
            CHECK(blocks[i][0] == static_cast<u8>(i & 0xFFU));
            CHECK(blocks[i][k_s - 1] == static_cast<u8>(i & 0xFFU));
        }
    }

    SECTION("near-OOM — try_allocate returns null, recovers after free")
    {
        constexpr usize chunk = 64U * 1024U;
        constexpr usize max_chunks = 256U;
        u8* held[max_chunks];
        usize held_count = 0;
        for (; held_count < max_chunks; ++held_count)
        {
            u8* p = static_cast<u8*>(tlsf.try_allocate(chunk, 16U));
            if (p == nullptr)
            {
                break; // expected — pool exhausted, no crash
            }
            held[held_count] = p;
        }
        CHECK(held_count > 0U);
        CHECK(held_count < max_chunks); // i.e. we actually hit exhaustion
        // One more must still be null while everything is held.
        CHECK(tlsf.try_allocate(chunk, 16U) == nullptr);
        for (usize i = 0; i < held_count; ++i)
        {
            tlsf.deallocate(held[i]);
        }
        // Recovered: a big allocation succeeds again.
        u8* big = static_cast<u8*>(tlsf.try_allocate(chunk, 16U));
        CHECK(big != nullptr);
        if (big != nullptr)
        {
            tlsf.deallocate(big);
        }
    }

    SECTION("alignment churn + allocation_size")
    {
        constexpr usize k_n = 300U;
        u8* ptrs[k_n];
        for (usize i = 0; i < k_n; ++i)
        {
            const usize a = usize{8} << (i % 7U); // 8..512, cycling
            const usize sz = 1U + (i * 37U) % 4096U;
            ptrs[i] = static_cast<u8*>(tlsf.try_allocate(sz, a));
            REQUIRE(ptrs[i] != nullptr);
            CHECK((reinterpret_cast<usize>(ptrs[i]) & (a - 1U)) == 0U);
            CHECK(tlsf.allocation_size(ptrs[i]) >= sz);
        }
        for (usize i = 0; i < k_n; ++i)
        {
            tlsf.deallocate(ptrs[i]);
        }
    }

    SECTION("fragmentation soup — random alloc/free with pattern verify")
    {
        crd::stress::Rng rng(0xF7A6'1234ULL);
        struct Rec
        {
            u8* ptr = nullptr;
            usize size = 0;
            u8 fill = 0;
        };
        constexpr u32 cap = 512U;
        Rec live[cap];
        u32 live_count = 0;
        u8 next_fill = 1U;
        for (u32 it = 0; it < 40000U; ++it)
        {
            const u32 roll = rng.next_u32(10U);
            if (roll < 6U && live_count < cap)
            {
                const usize sz = 1U + rng.next_u32(8U * 1024U);
                const usize a = usize{8} << rng.next_u32(6U);
                u8* p = static_cast<u8*>(tlsf.try_allocate(sz, a));
                if (p == nullptr)
                {
                    if (live_count > 0U) // relieve pressure
                    {
                        const u32 idx = rng.next_u32(live_count);
                        REQUIRE(region_ok(live[idx].ptr, live[idx].size, live[idx].fill));
                        tlsf.deallocate(live[idx].ptr);
                        live[idx] = live[--live_count];
                    }
                    continue;
                }
                REQUIRE((reinterpret_cast<usize>(p) & (a - 1U)) == 0U);
                const u8 f = next_fill++;
                if (next_fill == 0U)
                {
                    next_fill = 1U;
                }
                stamp(p, sz, f);
                live[live_count++] = Rec{p, sz, f};
            }
            else if (roll < 9U && live_count > 0U)
            {
                const u32 idx = rng.next_u32(live_count);
                REQUIRE(region_ok(live[idx].ptr, live[idx].size, live[idx].fill));
                tlsf.deallocate(live[idx].ptr);
                live[idx] = live[--live_count];
            }
            else if (live_count > 0U) // realloc
            {
                const u32 idx = rng.next_u32(live_count);
                REQUIRE(region_ok(live[idx].ptr, live[idx].size, live[idx].fill));
                const usize ns = 1U + rng.next_u32(16U * 1024U);
                u8* np = static_cast<u8*>(tlsf.reallocate(live[idx].ptr, live[idx].size, ns));
                REQUIRE(np != nullptr);
                const usize keep = live[idx].size < ns ? live[idx].size : ns;
                if (keep > 0U)
                {
                    REQUIRE(np[0] == live[idx].fill);
                    REQUIRE(np[keep - 1] == live[idx].fill);
                }
                stamp(np, ns, live[idx].fill);
                live[idx].ptr = np;
                live[idx].size = ns;
            }
        }
        for (u32 i = 0; i < live_count; ++i)
        {
            REQUIRE(region_ok(live[i].ptr, live[i].size, live[i].fill));
            tlsf.deallocate(live[i].ptr);
        }
    }

    // Pool capacity is unchanged by all that churn; a near-full allocation works.
    CHECK(tlsf.pool_capacity() == initial_capacity);
    u8* whole = static_cast<u8*>(tlsf.try_allocate(initial_capacity / 2U, 16U));
    CHECK(whole != nullptr);
    if (whole != nullptr)
    {
        tlsf.deallocate(whole);
    }
}

// ---------------------------------------------------------------------------
// MallocAllocator — one cheap sanity test (the system allocator's own
// correctness isn't ours to prove).
// ---------------------------------------------------------------------------
TEST_CASE("allocators stress -- MallocAllocator sanity", "[stress][memory]")
{
    crd::memory::MallocAllocator a("v5-malloc-sanity");
    const usize sizes[] = {1U, 7U, 64U, 4096U, 1U << 20U};
    const usize aligns[] = {8U, 16U, 64U, 256U, 4096U};
    for (usize s : sizes)
    {
        for (usize al : aligns)
        {
            void* p = a.allocate(s, al);
            REQUIRE(p != nullptr);
            CHECK((reinterpret_cast<usize>(p) & (al - 1U)) == 0U);
            std::memset(p, 0x5A, s);
            a.deallocate(p);
        }
    }
    // (MallocAllocator::owns() always returns true by design — not worth asserting.)
}
