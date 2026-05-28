// crd::containers::ConcurrentQueue<T> — single-threaded behaviour (FIFO order,
// full/empty, capacity, wrap-around). Concurrent MPMC stress lives in
// tests/stress/test_concurrent_queue_stress.cpp.

#include <crd/containers/concurrent_queue.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::usize;
using crd::containers::ConcurrentQueue;
} // namespace

TEST_CASE("ConcurrentQueue single-thread FIFO + full/empty", "[containers][concurrent_queue]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "cq-test");

    // Vyukov capacity-pow2 has N usable slots (no sentinel slot is sacrificed).
    constexpr usize cap = 8U;
    ConcurrentQueue<u64> q(cap, &alloc);

    CHECK(q.capacity() == cap);
    CHECK(q.empty());

    for (u64 i = 0; i < cap; ++i)
    {
        REQUIRE(q.try_push(i));
    }
    // Full now.
    CHECK_FALSE(q.try_push(999ULL));
    CHECK(q.size() == cap);

    // FIFO drain.
    for (u64 i = 0; i < cap; ++i)
    {
        u64 v = ~0ULL;
        REQUIRE(q.try_pop(v));
        CHECK(v == i);
    }
    CHECK(q.empty());
    u64 sink = 0;
    CHECK_FALSE(q.try_pop(sink));
}

TEST_CASE("ConcurrentQueue wrap-around across many laps", "[containers][concurrent_queue]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "cq-test");
    constexpr usize cap = 4U;
    ConcurrentQueue<u64> q(cap, &alloc);

    u64 next_push = 0;
    u64 next_pop = 0;
    for (int lap = 0; lap < 100; ++lap)
    {
        const usize burst = (lap % cap) + 1U; // 1..cap items per lap
        for (usize k = 0; k < burst; ++k)
        {
            REQUIRE(q.try_push(next_push++));
        }
        for (usize k = 0; k < burst; ++k)
        {
            u64 v = ~0ULL;
            REQUIRE(q.try_pop(v));
            CHECK(v == next_pop++);
        }
        CHECK(q.empty());
    }
}

TEST_CASE("ConcurrentQueue try_emplace forwards args", "[containers][concurrent_queue]")
{
    struct Pair
    {
        u32 a;
        u32 b;
    };
    static_assert(std::is_trivially_copyable_v<Pair>);

    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "cq-test");
    ConcurrentQueue<Pair> q(8U, &alloc);

    REQUIRE(q.try_emplace(Pair{1U, 2U}));
    REQUIRE(q.try_push(Pair{3U, 4U}));
    Pair p{};
    REQUIRE(q.try_pop(p));
    CHECK(p.a == 1U);
    CHECK(p.b == 2U);
    REQUIRE(q.try_pop(p));
    CHECK(p.a == 3U);
    CHECK(p.b == 4U);
    CHECK(q.empty());
}
