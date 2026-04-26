#include <crd/memory/memory.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <vector>

using namespace crd;
using namespace crd::memory;

// =============================================================================
// alignment.hpp
// =============================================================================

TEST_CASE("alignment: is_pow2", "[memory][alignment]")
{
    REQUIRE(is_pow2(1));
    REQUIRE(is_pow2(2));
    REQUIRE(is_pow2(4));
    REQUIRE(is_pow2(64));
    REQUIRE(is_pow2(1024));
    REQUIRE_FALSE(is_pow2(0));
    REQUIRE_FALSE(is_pow2(3));
    REQUIRE_FALSE(is_pow2(1000));
}

TEST_CASE("alignment: align_up rounds up correctly", "[memory][alignment]")
{
    REQUIRE(align_up(0, 16) == 0);
    REQUIRE(align_up(1, 16) == 16);
    REQUIRE(align_up(15, 16) == 16);
    REQUIRE(align_up(16, 16) == 16);
    REQUIRE(align_up(17, 16) == 32);
    REQUIRE(align_up(100, 64) == 128);
}

TEST_CASE("alignment: is_aligned", "[memory][alignment]")
{
    alignas(64) char buf[128] = {};
    REQUIRE(is_aligned(&buf[0], 16));
    REQUIRE(is_aligned(&buf[0], 32));
    REQUIRE(is_aligned(&buf[0], 64));
    REQUIRE_FALSE(is_aligned(&buf[1], 2));
}

// =============================================================================
// MallocAllocator
// =============================================================================

TEST_CASE("MallocAllocator: allocate returns aligned memory", "[memory][malloc]")
{
    MallocAllocator a;
    for (usize align : {usize(8), usize(16), usize(32), usize(64), usize(128)})
    {
        void* p = a.allocate(123, align);
        REQUIRE(p != nullptr);
        REQUIRE(is_aligned(p, align));
        a.deallocate(p);
    }
}

TEST_CASE("MallocAllocator: round-trip many allocations", "[memory][malloc]")
{
    MallocAllocator a;
    std::vector<void*> ptrs;
    for (int i = 0; i < 50; ++i)
    {
        void* p = a.allocate(static_cast<usize>(i + 1) * 8, 16);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }
    for (void* p : ptrs)
    {
        a.deallocate(p);
    }
}

TEST_CASE("MallocAllocator: deallocate(nullptr) is safe", "[memory][malloc]")
{
    MallocAllocator a;
    a.deallocate(nullptr);
    SUCCEED();
}

TEST_CASE("MallocAllocator: reallocate grows preserving content", "[memory][malloc]")
{
    MallocAllocator a;
    char* p = static_cast<char*>(a.allocate(16, 16));
    std::memcpy(p, "hello world!!!\0", 16);
    char* q = static_cast<char*>(a.reallocate(p, 16, 64, 16));
    REQUIRE(q != nullptr);
    REQUIRE(std::strcmp(q, "hello world!!!") == 0);
    a.deallocate(q);
}

TEST_CASE("MallocAllocator: reallocate(nullptr, 0, n) acts as allocate", "[memory][malloc]")
{
    MallocAllocator a;
    void* p = a.reallocate(nullptr, 0, 64, 16);
    REQUIRE(p != nullptr);
    a.deallocate(p);
}

TEST_CASE("MallocAllocator: reallocate to size 0 frees", "[memory][malloc]")
{
    MallocAllocator a;
    void* p = a.allocate(64, 16);
    void* q = a.reallocate(p, 64, 0, 16);
    REQUIRE(q == nullptr);
}

TEST_CASE("default_allocator returns a stable pointer", "[memory][default]")
{
    IAllocator* a = default_allocator();
    IAllocator* b = default_allocator();
    REQUIRE(a == b);
    REQUIRE(a != nullptr);
}

// =============================================================================
// LinearAllocator
// =============================================================================

TEST_CASE("LinearAllocator: hands out aligned chunks", "[memory][linear]")
{
    LinearAllocator a(1024);
    void* p1 = a.allocate(100, 16);
    void* p2 = a.allocate(200, 32);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(is_aligned(p1, 16));
    REQUIRE(is_aligned(p2, 32));
    REQUIRE(p2 > p1);
    REQUIRE(a.used() >= 300);
    REQUIRE(a.owns(p1));
    REQUIRE(a.owns(p2));
}

TEST_CASE("LinearAllocator: returns nullptr on exhaustion", "[memory][linear]")
{
    LinearAllocator a(64);
    void* p = a.allocate(32, 16);
    REQUIRE(p != nullptr);
    void* q = a.allocate(64, 16); // would exceed
    REQUIRE(q == nullptr);
}

TEST_CASE("LinearAllocator: reset() reclaims everything", "[memory][linear]")
{
    LinearAllocator a(256);
    void* p = a.allocate(128, 16);
    REQUIRE(p != nullptr);
    REQUIRE(a.used() >= 128);
    a.reset();
    REQUIRE(a.used() == 0);
    void* q = a.allocate(200, 16);
    REQUIRE(q != nullptr);
}

TEST_CASE("LinearAllocator: deallocate is a no-op", "[memory][linear]")
{
    LinearAllocator a(256);
    void* p = a.allocate(64, 16);
    const usize before = a.used();
    a.deallocate(p);
    REQUIRE(a.used() == before);
}

TEST_CASE("LinearScope rolls back to saved offset", "[memory][linear][scope]")
{
    LinearAllocator a(1024);
    void* outer = a.allocate(64, 16);
    REQUIRE(outer != nullptr);
    const usize before = a.used();
    {
        LinearScope scope(a);
        void* inner1 = a.allocate(64, 16);
        void* inner2 = a.allocate(64, 16);
        REQUIRE(inner1 != nullptr);
        REQUIRE(inner2 != nullptr);
        REQUIRE(a.used() > before);
    }
    REQUIRE(a.used() == before);
}

TEST_CASE("LinearAllocator: reset_to specific offset", "[memory][linear]")
{
    LinearAllocator a(512);
    a.allocate(64, 16);
    const usize mid = a.used();
    a.allocate(64, 16);
    REQUIRE(a.used() > mid);
    a.reset_to(mid);
    REQUIRE(a.used() == mid);
}

// =============================================================================
// StackAllocator
// =============================================================================

TEST_CASE("StackAllocator: marker rolls back", "[memory][stack]")
{
    StackAllocator s(1024);
    auto m0 = s.mark();
    void* a = s.allocate(64, 16);
    REQUIRE(a != nullptr);
    auto m1 = s.mark();
    void* b = s.allocate(128, 16);
    REQUIRE(b != nullptr);
    REQUIRE(s.used() > 64);

    s.reset_to(m1);
    REQUIRE(s.used() <= 128); // a still allocated, b freed

    s.reset_to(m0);
    REQUIRE(s.used() == 0);
}

TEST_CASE("StackAllocator: nested StackScope", "[memory][stack][scope]")
{
    StackAllocator s(1024);
    void* outer = s.allocate(64, 16);
    REQUIRE(outer != nullptr);
    const usize before = s.used();
    {
        StackScope scope1(s);
        s.allocate(64, 16);
        {
            StackScope scope2(s);
            s.allocate(128, 16);
            REQUIRE(s.used() > before + 64);
        }
        // scope2 freed
    }
    REQUIRE(s.used() == before);
}

TEST_CASE("StackAllocator: exhaustion returns nullptr", "[memory][stack]")
{
    StackAllocator s(64);
    REQUIRE(s.allocate(32, 16) != nullptr);
    REQUIRE(s.allocate(64, 16) == nullptr);
}

// =============================================================================
// PoolAllocator
// =============================================================================

TEST_CASE("PoolAllocator: hands out distinct slots", "[memory][pool]")
{
    PoolAllocator pool(64, 8, 16);
    REQUIRE(pool.slot_size() == 64);
    REQUIRE(pool.slot_count() == 8);

    std::vector<void*> ptrs;
    for (usize i = 0; i < 8; ++i)
    {
        void* p = pool.allocate(64, 16);
        REQUIRE(p != nullptr);
        REQUIRE(is_aligned(p, 16));
        REQUIRE(pool.owns(p));
        ptrs.push_back(p);
    }

    REQUIRE(pool.slots_in_use() == 8);
    REQUIRE(pool.slots_free() == 0);

    // No more slots.
    REQUIRE(pool.allocate(64, 16) == nullptr);

    // Free one, get one.
    pool.deallocate(ptrs[3]);
    REQUIRE(pool.slots_in_use() == 7);
    void* recycled = pool.allocate(64, 16);
    REQUIRE(recycled != nullptr);
    // Most-recently-freed gets reused (LIFO).
    REQUIRE(recycled == ptrs[3]);
}

TEST_CASE("PoolAllocator: slot_size is rounded up to alignment", "[memory][pool]")
{
    PoolAllocator pool(33, 4, 16); // 33 -> 48
    REQUIRE(pool.slot_size() == 48);
}

TEST_CASE("PoolAllocator: owns rejects out-of-range and interior pointers", "[memory][pool]")
{
    PoolAllocator pool(64, 4, 16);
    void* p = pool.allocate(64, 16);
    REQUIRE(pool.owns(p));
    char* q = static_cast<char*>(p) + 8; // interior
    REQUIRE_FALSE(pool.owns(q));
    int stack_local = 0;
    REQUIRE_FALSE(pool.owns(&stack_local));
    pool.deallocate(p);
}

TEST_CASE("PoolAllocator: deallocate(nullptr) is safe", "[memory][pool]")
{
    PoolAllocator pool(64, 4, 16);
    pool.deallocate(nullptr);
    SUCCEED();
}

// =============================================================================
// construct/destroy helpers
// =============================================================================

namespace
{
struct DtorCounter
{
    int* counter;
    explicit DtorCounter(int* c) : counter(c) {}
    ~DtorCounter() { ++(*counter); }
};
} // namespace

TEST_CASE("construct/destroy calls dtor", "[memory][construct]")
{
    int dtor_count = 0;
    auto* alloc = default_allocator();

    DtorCounter* p = construct<DtorCounter>(*alloc, &dtor_count);
    REQUIRE(p != nullptr);
    REQUIRE(dtor_count == 0);
    destroy(*alloc, p);
    REQUIRE(dtor_count == 1);
}

TEST_CASE("construct_array / destroy_array", "[memory][construct]")
{
    int dtor_count = 0;
    auto* alloc = default_allocator();

    DtorCounter* arr = construct_array<DtorCounter>(*alloc, 5, &dtor_count);
    REQUIRE(arr != nullptr);
    destroy_array(*alloc, arr, 5);
    REQUIRE(dtor_count == 5);
}

// =============================================================================
// Stats
// =============================================================================

#if defined(CRD_DEBUG)
TEST_CASE("MemoryStats tracks alloc/dealloc in debug builds", "[memory][stats]")
{
    MallocAllocator a;
    const auto before = a.stats().snapshot();
    void* p = a.allocate(128, 16);
    void* q = a.allocate(64, 16);
    const auto mid = a.stats().snapshot();
    REQUIRE(mid.alloc_count == before.alloc_count + 2);
    REQUIRE(mid.bytes_in_use >= before.bytes_in_use + 192);
    a.deallocate(p);
    a.deallocate(q);
    const auto after = a.stats().snapshot();
    REQUIRE(after.dealloc_count == before.dealloc_count + 2);
}
#endif

TEST_CASE("Stats snapshot is callable in any build", "[memory][stats]")
{
    MallocAllocator a;
    const auto snap = a.stats().snapshot();
    (void)snap;
    SUCCEED();
}
