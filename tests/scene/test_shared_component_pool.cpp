// Phase 3.0 v1m4b1 — SharedComponentPool unit tests (ADR-0058 pillar 5).
//
// Substrate-only coverage: just the data structure (alloc, refcount, freelist,
// grow). Integration with SparseSetStorage::Pool lands in v1m4b2; CoW
// write-path tests land in v1m4b3.

#include <crd/memory/allocator.hpp>
#include <crd/scene/shared_component_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using crd::scene::SharedComponentPool;

namespace
{
// 12-byte test payload (Vec3-shaped). Trivially copyable.
struct Payload
{
    crd::u32 a;
    crd::u32 b;
    crd::u32 c;
};
} // namespace

TEST_CASE("SharedComponentPool default state is empty", "[shared-pool][substrate]")
{
    SharedComponentPool pool{crd::memory::default_allocator(), sizeof(Payload), alignof(Payload)};
    CHECK(pool.live_count() == 0U);
    CHECK(pool.capacity() == 0U);
    CHECK(pool.entry_size() == sizeof(Payload));
}

TEST_CASE("SharedComponentPool acquire returns idx 0 with refcount 1", "[shared-pool][acquire]")
{
    SharedComponentPool pool{crd::memory::default_allocator(), sizeof(Payload), alignof(Payload)};
    Payload p{1U, 2U, 3U};
    const crd::u32 idx = pool.acquire(&p);
    CHECK(idx == 0U);
    CHECK(pool.refcount(0U) == 1U);
    CHECK(pool.live_count() == 1U);
    // Read the bytes back.
    const Payload* readback = reinterpret_cast<const Payload*>(pool.entry_bytes(idx));
    CHECK(readback->a == 1U);
    CHECK(readback->b == 2U);
    CHECK(readback->c == 3U);
}

TEST_CASE("Multiple acquires hand out increasing idxs", "[shared-pool][acquire]")
{
    SharedComponentPool pool{crd::memory::default_allocator(), sizeof(Payload), alignof(Payload)};
    Payload p1{10U, 20U, 30U};
    Payload p2{100U, 200U, 300U};
    Payload p3{1000U, 2000U, 3000U};
    const crd::u32 i1 = pool.acquire(&p1);
    const crd::u32 i2 = pool.acquire(&p2);
    const crd::u32 i3 = pool.acquire(&p3);
    CHECK(i1 == 0U);
    CHECK(i2 == 1U);
    CHECK(i3 == 2U);
    CHECK(pool.live_count() == 3U);
    // Each entry preserves its own bytes.
    CHECK(reinterpret_cast<const Payload*>(pool.entry_bytes(i1))->a == 10U);
    CHECK(reinterpret_cast<const Payload*>(pool.entry_bytes(i2))->a == 100U);
    CHECK(reinterpret_cast<const Payload*>(pool.entry_bytes(i3))->a == 1000U);
}

TEST_CASE("retain bumps refcount; release decrements; release-to-zero frees", "[shared-pool][refcount]")
{
    SharedComponentPool pool{crd::memory::default_allocator(), sizeof(Payload), alignof(Payload)};
    Payload p{42U, 0U, 0U};
    const crd::u32 idx = pool.acquire(&p);
    CHECK(pool.refcount(idx) == 1U);

    pool.retain(idx);
    pool.retain(idx);
    CHECK(pool.refcount(idx) == 3U);
    CHECK(pool.live_count() == 1U);  // still 1 live entry, just refcount grew

    pool.release(idx);
    CHECK(pool.refcount(idx) == 2U);
    pool.release(idx);
    CHECK(pool.refcount(idx) == 1U);
    pool.release(idx);
    CHECK(pool.refcount(idx) == 0U);
    CHECK(pool.live_count() == 0U);
}

TEST_CASE("Released slots are reused via freelist", "[shared-pool][freelist]")
{
    SharedComponentPool pool{crd::memory::default_allocator(), sizeof(Payload), alignof(Payload)};
    Payload p1{1U, 0U, 0U};
    Payload p2{2U, 0U, 0U};
    Payload p3{3U, 0U, 0U};
    const crd::u32 i1 = pool.acquire(&p1);
    const crd::u32 i2 = pool.acquire(&p2);
    const crd::u32 i3 = pool.acquire(&p3);
    CHECK(i1 == 0U);
    CHECK(i2 == 1U);
    CHECK(i3 == 2U);

    // Release middle slot.
    pool.release(i2);
    CHECK(pool.refcount(i2) == 0U);
    CHECK(pool.live_count() == 2U);

    // Next acquire reuses slot 1 (LIFO freelist).
    Payload p4{4U, 0U, 0U};
    const crd::u32 i4 = pool.acquire(&p4);
    CHECK(i4 == i2);
    CHECK(pool.refcount(i4) == 1U);
    CHECK(reinterpret_cast<const Payload*>(pool.entry_bytes(i4))->a == 4U);
    CHECK(pool.live_count() == 3U);
}

TEST_CASE("Pool grows past initial capacity", "[shared-pool][grow]")
{
    SharedComponentPool pool{crd::memory::default_allocator(), sizeof(Payload), alignof(Payload)};
    constexpr crd::u32 count = 50U;  // > initial 8 slots; should grow ≥ 64.
    crd::containers::Array<crd::u32> idxs{crd::memory::default_allocator()};
    for (crd::u32 i = 0; i < count; ++i)
    {
        Payload p{i, i + 1U, i + 2U};
        idxs.push_back(pool.acquire(&p));
    }
    CHECK(pool.live_count() == count);
    CHECK(pool.capacity() >= count);
    // All bytes intact through grow.
    for (crd::u32 i = 0; i < count; ++i)
    {
        const Payload* p = reinterpret_cast<const Payload*>(pool.entry_bytes(idxs[i]));
        CHECK(p->a == i);
        CHECK(p->b == i + 1U);
        CHECK(p->c == i + 2U);
    }
}

TEST_CASE("Refcount of never-acquired idx returns 0", "[shared-pool][safety]")
{
    SharedComponentPool pool{crd::memory::default_allocator(), sizeof(Payload), alignof(Payload)};
    CHECK(pool.refcount(0U) == 0U);
    CHECK(pool.refcount(999U) == 0U);
    Payload p{0U, 0U, 0U};
    (void)pool.acquire(&p);
    CHECK(pool.refcount(0U) == 1U);
    CHECK(pool.refcount(1U) == 0U);
}
