// test_virtual_memory_allocator.cpp — crd::memory::VirtualMemoryAllocator (ADR-0085 S2).
//
// Covers the stable-address bump arena over crd::vm: lazy commit-on-demand in
// commit-block multiples, alignment, deallocate-is-no-op + owns() bounds,
// mark/reset_to LIFO pop, reset (keeps committed), purge (decommits the tail,
// RSS drops), reallocate top-in-place vs copy, the try_allocate OOM path (nullptr,
// NOT a death-test on allocate's fatal), initial_commit pre-warm, zero-on-commit,
// and the stable-address-survives-purge invariant. Reservations are kept small
// (256 MiB) per fixture; one case exercises the 64 GiB default.

#include <crd/core/types.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/memory/allocators/virtual_memory_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace
{
using crd::memory::VirtualMemoryAllocator;
using Config = VirtualMemoryAllocator::Config;

constexpr crd::usize kMiB = crd::usize{1} << 20;

[[nodiscard]] bool is_aligned(const void* p, crd::usize a) noexcept
{
    return (reinterpret_cast<std::uintptr_t>(p) & (a - 1)) == 0;
}

[[nodiscard]] Config small_cfg() noexcept
{
    Config c;
    c.reserve_bytes = 256 * kMiB;
    c.commit_block  = crd::usize{64} << 10; // 64 KiB
    return c;
}
} // namespace

TEST_CASE("vma construct reserves address space, commits nothing", "[vmalloc]")
{
    VirtualMemoryAllocator a(small_cfg());
    REQUIRE(a.base() != nullptr);
    REQUIRE(a.reserved_bytes() >= 256 * kMiB);
    REQUIRE(a.committed_bytes() == 0); // lazy
    REQUIRE(a.used_bytes() == 0);
}

TEST_CASE("vma allocate bumps and commits lazily in commit-block multiples", "[vmalloc]")
{
    const crd::usize block = crd::usize{64} << 10;
    VirtualMemoryAllocator a(small_cfg());

    void* p = a.allocate(1);
    REQUIRE(p == a.base());
    REQUIRE(a.used_bytes() == 1);
    REQUIRE(a.committed_bytes() == block); // one block committed for a 1-byte ask

    // Grow past the first block; committed jumps to the next multiple.
    void* q = a.allocate(100 * 1024);
    REQUIRE(q != nullptr);
    REQUIRE(a.committed_bytes() >= a.used_bytes());
    REQUIRE(a.committed_bytes() % block == 0);
    REQUIRE(a.committed_bytes() - a.used_bytes() < block); // tight: no over-commit beyond one block
}

TEST_CASE("vma honors alignment", "[vmalloc]")
{
    VirtualMemoryAllocator a(small_cfg());
    REQUIRE(is_aligned(a.allocate(3, 16), 16));
    REQUIRE(is_aligned(a.allocate(7, 64), 64));
    REQUIRE(is_aligned(a.allocate(1, 256), 256));
    REQUIRE(is_aligned(a.allocate(4096, 4096), 4096));
}

TEST_CASE("vma deallocate is a no-op and owns() bounds the handed-out range", "[vmalloc]")
{
    VirtualMemoryAllocator a(small_cfg());
    void* p = a.allocate(128);
    REQUIRE(a.owns(p));
    REQUIRE_FALSE(a.owns(static_cast<const crd::u8*>(a.base()) - 1)); // before base
    REQUIRE_FALSE(a.owns(static_cast<const crd::u8*>(a.base()) + a.used_bytes())); // one past top

    const crd::usize before = a.used_bytes();
    a.deallocate(p);                  // no-op
    a.deallocate(nullptr);            // safe
    REQUIRE(a.used_bytes() == before); // nothing reclaimed by deallocate
}

TEST_CASE("vma mark and reset_to pop LIFO and reuse the popped span", "[vmalloc]")
{
    VirtualMemoryAllocator a(small_cfg());
    a.allocate(1000);
    const auto m = a.mark();
    void* b = a.allocate(2000);
    REQUIRE(a.used_bytes() > m);

    a.reset_to(m);
    REQUIRE(a.used_bytes() == m);

    void* b2 = a.allocate(2000); // reuses the popped region at the same address
    REQUIRE(b2 == b);
}

TEST_CASE("vma reset wipes use but keeps pages committed", "[vmalloc]")
{
    VirtualMemoryAllocator a(small_cfg());
    a.allocate(50 * 1024);
    const crd::usize committed = a.committed_bytes();
    REQUIRE(committed > 0);

    a.reset();
    REQUIRE(a.used_bytes() == 0);
    REQUIRE(a.committed_bytes() == committed); // reset is logical, not physical
}

TEST_CASE("vma purge decommits the tail and recommit re-zeroes", "[vmalloc]")
{
    VirtualMemoryAllocator a(small_cfg());
    auto* p = static_cast<crd::u8*>(a.allocate(200 * 1024));
    for (crd::usize i = 0; i < 200 * 1024; ++i) { p[i] = 0xCD; }
    const crd::usize committed_before = a.committed_bytes();
    REQUIRE(committed_before > 0);

    a.reset_and_purge();
    REQUIRE(a.used_bytes() == 0);
    REQUIRE(a.committed_bytes() < committed_before); // RSS handed back

    auto* q = static_cast<crd::u8*>(a.allocate(200 * 1024));
    REQUIRE(q == p);             // stable address
    REQUIRE(q[0] == 0);          // recommitted pages read back zero
    REQUIRE(q[200 * 1024 - 1] == 0);
}

TEST_CASE("vma reallocate grows the top allocation in place", "[vmalloc]")
{
    VirtualMemoryAllocator a(small_cfg());
    auto* p = static_cast<crd::u8*>(a.allocate(64));
    for (int i = 0; i < 64; ++i) { p[i] = static_cast<crd::u8>(i); }

    void* grown = a.reallocate(p, 64, 4096);
    REQUIRE(grown == p); // top allocation grows without moving
    auto* g = static_cast<crd::u8*>(grown);
    for (int i = 0; i < 64; ++i) { REQUIRE(g[i] == static_cast<crd::u8>(i)); } // data preserved
}

TEST_CASE("vma reallocate copies when not the top allocation", "[vmalloc]")
{
    VirtualMemoryAllocator a(small_cfg());
    auto* p = static_cast<crd::u8*>(a.allocate(64));
    for (int i = 0; i < 64; ++i) { p[i] = static_cast<crd::u8>(0xA0 + i); }
    a.allocate(64); // p is no longer the top

    auto* moved = static_cast<crd::u8*>(a.reallocate(p, 64, 128));
    REQUIRE(moved != p); // had to relocate
    for (int i = 0; i < 64; ++i) { REQUIRE(moved[i] == static_cast<crd::u8>(0xA0 + i)); }
}

TEST_CASE("vma try_allocate returns nullptr past the reservation (graceful OOM)", "[vmalloc]")
{
    Config c;
    c.reserve_bytes = kMiB; // tiny reservation
    VirtualMemoryAllocator a(c);

    REQUIRE(a.try_allocate(0) == nullptr);          // zero size
    REQUIRE(a.try_allocate(8 * kMiB) == nullptr);   // larger than the whole reservation
    void* p = a.try_allocate(64 * 1024);            // comfortably fits
    REQUIRE(p != nullptr);
    REQUIRE(a.try_allocate(a.reserved_bytes()) == nullptr); // can't fit on top of p
}

TEST_CASE("vma rejects an allocation whose alignment rounds past the reservation end", "[vmalloc]")
{
    Config c;
    c.reserve_bytes = 4096; // exactly one page
    VirtualMemoryAllocator a(c);
    REQUIRE(a.reserved_bytes() == 4096);

    // Fill to 6 bytes short of the end, leaving an unaligned tail.
    REQUIRE(a.try_allocate(4090, 1) != nullptr);
    REQUIRE(a.used_bytes() == 4090);

    // A 16-aligned 1-byte ask rounds the offset up to 4096 -> past the end -> nullptr.
    REQUIRE(a.try_allocate(1, 16) == nullptr);
    // But the exact remaining 6 bytes at alignment 1 still fit.
    REQUIRE(a.try_allocate(6, 1) != nullptr);
    REQUIRE(a.used_bytes() == 4096);
}

TEST_CASE("vma initial_commit_bytes pre-warms committed pages", "[vmalloc]")
{
    Config c   = small_cfg();
    c.initial_commit_bytes = 2 * kMiB;
    VirtualMemoryAllocator a(c);
    REQUIRE(a.committed_bytes() >= 2 * kMiB);
    REQUIRE(a.used_bytes() == 0); // committed, not allocated
}

TEST_CASE("vma fresh commit reads back zero", "[vmalloc]")
{
    VirtualMemoryAllocator a(small_cfg());
    auto* p = static_cast<crd::u8*>(a.allocate(8 * 1024));
    for (crd::usize i = 0; i < 8 * 1024; ++i) { REQUIRE(p[i] == 0); }
}

TEST_CASE("vma low allocation survives a purge of higher pages (stable address)", "[vmalloc]")
{
    VirtualMemoryAllocator a(small_cfg());
    auto* keep = static_cast<crd::u8*>(a.allocate(4096));
    keep[0]    = 0x42;
    keep[4095] = 0x24;
    const auto m = a.mark();

    a.allocate(8 * kMiB); // grow the committed high-water mark well past `keep`
    a.reset_to(m);        // logically free the big block
    a.purge();            // decommit the now-unused tail

    REQUIRE(keep[0] == 0x42);    // low data intact at the same address
    REQUIRE(keep[4095] == 0x24);
}

TEST_CASE("vma reserves the 64 GiB default address range", "[vmalloc]")
{
    VirtualMemoryAllocator a; // default Config: 64 GiB reserve
    REQUIRE(a.reserved_bytes() >= (crd::usize{64} << 30));
    REQUIRE(a.committed_bytes() == 0);
    void* p = a.allocate(1);
    REQUIRE(p == a.base());
}
