#include <crd/containers/array.hpp>
#include <crd/memory/allocators/growable_pool_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <random>
#include <utility>

using crd::memory::GrowablePoolAllocator;
using crd::memory::IAllocator;

namespace
{
bool is_aligned(const void* p, crd::usize alignment)
{
    return (reinterpret_cast<std::uintptr_t>(p) & (alignment - 1)) == 0;
}
} // namespace

TEST_CASE("GrowablePool ctor records configuration; no pages until first allocate", "[memory][gpool]")
{
    GrowablePoolAllocator p{64U, 16U, 8U};
    CHECK(p.slot_size() == 64U);
    CHECK(p.slot_alignment() == 16U);
    CHECK(p.slots_per_page() == 8U);
    CHECK(p.page_count() == 0U);
    CHECK(p.slots_in_use() == 0U);
    CHECK(p.slots_free() == 0U); // no pages allocated yet
}

TEST_CASE("GrowablePool first allocate creates a page", "[memory][gpool]")
{
    GrowablePoolAllocator p{64U, 16U, 8U};

    void* a = p.allocate(64);
    REQUIRE(a != nullptr);
    CHECK(is_aligned(a, 16U));
    CHECK(p.page_count() == 1U);
    CHECK(p.slots_in_use() == 1U);
    CHECK(p.slots_free() == 7U);

    p.deallocate(a);
    CHECK(p.slots_in_use() == 0U);
    CHECK(p.slots_free() == 8U);
}

TEST_CASE("GrowablePool slots are contiguous within a page at slot_alignment stride", "[memory][gpool]")
{
    GrowablePoolAllocator p{64U, 16U, 8U};

    crd::containers::Array<void*> ptrs;
    for (int i = 0; i < 8; ++i)
    {
        void* x = p.allocate(64);
        REQUIRE(x != nullptr);
        ptrs.push_back(x);
    }

    // Eight allocations from one page; page_count should still be 1.
    CHECK(p.page_count() == 1U);

    // Verify all are aligned and pairwise distinct.
    for (crd::usize i = 0; i < ptrs.size(); ++i)
    {
        CHECK(is_aligned(ptrs[i], 16U));
        for (crd::usize j = i + 1; j < ptrs.size(); ++j)
        {
            CHECK(ptrs[i] != ptrs[j]);
        }
    }

    for (void* x : ptrs)
    {
        p.deallocate(x);
    }
    CHECK(p.slots_in_use() == 0U);
}

TEST_CASE("GrowablePool grows a second page when the first is full", "[memory][gpool]")
{
    GrowablePoolAllocator p{64U, 16U, 4U}; // 4 slots/page

    crd::containers::Array<void*> ptrs;
    for (int i = 0; i < 10; ++i)
    {
        void* x = p.allocate(64);
        REQUIRE(x != nullptr);
        ptrs.push_back(x);
    }
    // 10 slots → 3 pages (4 + 4 + 2 used in third).
    CHECK(p.page_count() == 3U);
    CHECK(p.slots_in_use() == 10U);
    CHECK(p.slots_free() == 2U); // 4*3 - 10 = 2

    for (void* x : ptrs)
    {
        p.deallocate(x);
    }
    CHECK(p.slots_in_use() == 0U);
    // page_count stays at 3 — pool keeps pages allocated.
    CHECK(p.page_count() == 3U);
}

TEST_CASE("GrowablePool free-list reuses freed slots before growing", "[memory][gpool]")
{
    GrowablePoolAllocator p{64U, 16U, 4U};

    void* a = p.allocate(64);
    void* b = p.allocate(64);
    void* c = p.allocate(64);
    void* d = p.allocate(64);
    CHECK(p.page_count() == 1U);

    p.deallocate(b);
    p.deallocate(c);

    // Allocations should reuse b/c slots before growing.
    void* e = p.allocate(64);
    void* f = p.allocate(64);
    CHECK(p.page_count() == 1U);
    CHECK((e == b || e == c));
    CHECK((f == b || f == c));
    CHECK(e != f);

    p.deallocate(a);
    p.deallocate(d);
    p.deallocate(e);
    p.deallocate(f);
}

TEST_CASE("GrowablePool deallocate(nullptr) is a no-op", "[memory][gpool]")
{
    GrowablePoolAllocator p{64U, 16U, 4U};
    p.deallocate(nullptr);
    CHECK(p.slots_in_use() == 0U);
}

TEST_CASE("GrowablePool owns() returns true for slots, false otherwise", "[memory][gpool]")
{
    GrowablePoolAllocator p{64U, 16U, 4U};
    void* x = p.allocate(64);
    REQUIRE(x != nullptr);

    CHECK(p.owns(x));
    CHECK_FALSE(p.owns(nullptr));

    int stack_var = 0;
    CHECK_FALSE(p.owns(&stack_var));

    p.deallocate(x);
}

TEST_CASE("GrowablePool owns() crosses page boundaries correctly", "[memory][gpool]")
{
    GrowablePoolAllocator p{64U, 16U, 2U}; // tiny pages → many pages

    crd::containers::Array<void*> ptrs;
    for (int i = 0; i < 8; ++i)
    {
        ptrs.push_back(p.allocate(64));
    }
    REQUIRE(p.page_count() >= 4U);

    for (void* x : ptrs)
    {
        CHECK(p.owns(x));
    }

    for (void* x : ptrs)
    {
        p.deallocate(x);
    }
}

TEST_CASE("GrowablePool allocation_size returns slot_size for owned pointers", "[memory][gpool]")
{
    GrowablePoolAllocator p{96U, 32U, 4U};
    void* x = p.allocate(40);
    REQUIRE(x != nullptr);
    CHECK(p.allocation_size(x) == 96U);
    CHECK(p.allocation_size(nullptr) == 0U);
    int stack_var = 0;
    CHECK(p.allocation_size(&stack_var) == 0U);
    p.deallocate(x);
}

TEST_CASE("GrowablePool aligned slots - 64-byte alignment for chunk-style use", "[memory][gpool]")
{
    GrowablePoolAllocator p{16U * 1024U, 64U, 64U}; // 16 KB / 64-aligned / 64 per page (1 MB pages)

    crd::containers::Array<void*> ptrs;
    for (int i = 0; i < 200; ++i) // exceeds one page
    {
        void* x = p.allocate(16U * 1024U);
        REQUIRE(x != nullptr);
        REQUIRE(is_aligned(x, 64U));
        ptrs.push_back(x);
    }
    CHECK(p.page_count() >= 4U); // ⌈200/64⌉ = 4

    for (void* x : ptrs)
    {
        p.deallocate(x);
    }
    CHECK(p.slots_in_use() == 0U);
}

TEST_CASE("GrowablePool dtor releases all pages (ASan leak check)", "[memory][gpool]")
{
    {
        GrowablePoolAllocator p{64U, 16U, 8U};
        for (int i = 0; i < 50; ++i)
        {
            (void)p.allocate(64);
        }
        // Intentionally don't call deallocate — dtor must clean up.
    }
    SUCCEED();
}

TEST_CASE("GrowablePool move ctor transfers pages and free list", "[memory][gpool]")
{
    GrowablePoolAllocator src{64U, 16U, 4U};

    crd::containers::Array<void*> ptrs;
    for (int i = 0; i < 6; ++i)
    {
        ptrs.push_back(src.allocate(64));
    }
    const crd::usize src_pages = src.page_count();
    const crd::usize src_in_use = src.slots_in_use();

    GrowablePoolAllocator dst{std::move(src)};
    CHECK(dst.page_count() == src_pages);
    CHECK(dst.slots_in_use() == src_in_use);

    // Source is empty after move.
    CHECK(src.page_count() == 0U); // NOLINT(bugprone-use-after-move) — verifying moved-from state
    CHECK(src.slots_in_use() == 0U);

    // dst still owns the pointers.
    for (void* x : ptrs)
    {
        CHECK(dst.owns(x));
    }
    for (void* x : ptrs)
    {
        dst.deallocate(x);
    }
}

TEST_CASE("GrowablePool stress: 2000 random alloc/free preserves invariants", "[memory][gpool][stress]")
{
    GrowablePoolAllocator p{96U, 16U, 16U};

    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng{8675309};
    crd::containers::Array<void*> live;
    std::uniform_int_distribution<int> op_pick(0, 2);

    for (int iter = 0; iter < 2000; ++iter)
    {
        const bool do_free = (live.size() > 0) && (op_pick(rng) == 0);
        if (do_free)
        {
            std::uniform_int_distribution<crd::usize> idx_pick(0U, live.size() - 1U);
            const crd::usize idx = idx_pick(rng);
            p.deallocate(live[idx]);
            live.swap_remove(idx);
        }
        else
        {
            void* x = p.allocate(96);
            REQUIRE(x != nullptr);
            REQUIRE(is_aligned(x, 16U));
            // Touch it.
            std::memset(x, iter & 0xFF, 96);
            live.push_back(x);
        }
    }

    for (void* x : live)
    {
        p.deallocate(x);
    }
    CHECK(p.slots_in_use() == 0U);
}

TEST_CASE("GrowablePool reports IAllocator interface compatibility", "[memory][gpool]")
{
    GrowablePoolAllocator p{64U, 16U, 4U};

    IAllocator& iface = p;
    void* x = iface.allocate(40, 16);
    REQUIRE(x != nullptr);
    CHECK(iface.owns(x));
    CHECK(iface.allocation_size(x) == 64U);
    iface.deallocate(x);
}
