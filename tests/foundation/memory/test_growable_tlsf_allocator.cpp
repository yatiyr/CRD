#include <crd/containers/array.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/allocators/virtual_memory_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

using crd::memory::GrowableTlsfAllocator;

namespace
{
bool is_aligned(const void* p, crd::usize alignment)
{
    return (reinterpret_cast<std::uintptr_t>(p) & (alignment - 1)) == 0;
}
} // namespace

TEST_CASE("GrowableTlsfAllocator serves a basic alloc/dealloc and owns it", "[memory][growable-tlsf]")
{
    GrowableTlsfAllocator a;
    void*                 p = a.allocate(1024, 16);
    REQUIRE(p != nullptr);
    REQUIRE(is_aligned(p, 16));
    REQUIRE(a.owns(p));
    REQUIRE(a.num_chunks() == 1);
    std::memset(p, 0xAB, 1024); // ASan: in-bounds
    a.deallocate(p);
}

TEST_CASE("GrowableTlsfAllocator grows new chunks when one fills", "[memory][growable-tlsf]")
{
    // Tiny nominal chunk so a handful of allocations forces growth past one pool.
    const crd::usize         chunk = crd::memory::TlsfAllocator::min_pool_size() + (64U * 1024U);
    GrowableTlsfAllocator    a(chunk);
    crd::containers::Array<void*> ptrs(crd::memory::default_allocator()); // bookkeeping only; not the SUT
    // Allocate enough 16 KB blocks to overflow several chunks.
    for (int i = 0; i < 64; ++i)
    {
        void* p = a.allocate(16U * 1024U, 16);
        REQUIRE(p != nullptr);
        REQUIRE(a.owns(p));
        std::memset(p, i & 0xFF, 16U * 1024U);
        ptrs.push_back(p);
    }
    REQUIRE(a.num_chunks() > 1); // genuinely grew
    for (void* p : ptrs) { a.deallocate(p); }
}

TEST_CASE("GrowableTlsfAllocator serves an allocation larger than the nominal chunk", "[memory][growable-tlsf]")
{
    const crd::usize      chunk = crd::memory::TlsfAllocator::min_pool_size() + (64U * 1024U); // small nominal
    GrowableTlsfAllocator a(chunk);
    const crd::usize      big = 8U * 1024U * 1024U; // 8 MB ≫ nominal chunk
    void*                 p   = a.allocate(big, 64);
    REQUIRE(p != nullptr);
    REQUIRE(is_aligned(p, 64));
    REQUIRE(a.owns(p));
    std::memset(p, 0x5A, big); // ASan: the bespoke chunk really is `big` bytes
    a.deallocate(p);
}

TEST_CASE("GrowableTlsfAllocator reallocate preserves contents (incl. cross-chunk growth)", "[memory][growable-tlsf]")
{
    const crd::usize      chunk = crd::memory::TlsfAllocator::min_pool_size() + (64U * 1024U);
    GrowableTlsfAllocator a(chunk);
    const crd::usize      n0 = 4096;
    auto*                 p  = static_cast<unsigned char*>(a.allocate(n0, 16));
    for (crd::usize i = 0; i < n0; ++i) { p[i] = static_cast<unsigned char>(i & 0xFF); }
    // Grow well past the nominal chunk so the realloc must move to a fresh chunk.
    const crd::usize n1 = 2U * 1024U * 1024U;
    auto*            q  = static_cast<unsigned char*>(a.reallocate(p, n0, n1, 16));
    REQUIRE(q != nullptr);
    REQUIRE(a.owns(q));
    for (crd::usize i = 0; i < n0; ++i) { REQUIRE(q[i] == static_cast<unsigned char>(i & 0xFF)); } // preserved
    a.deallocate(q);
}

TEST_CASE("GrowableTlsfAllocator over a VirtualMemoryAllocator parent is malloc-free (ADR-0085 S3)",
          "[memory][growable-tlsf][vmalloc]")
{
    // The open-world malloc-free general heap: a growable TLSF whose chunks come
    // from a stable-address VM reservation instead of malloc. The VM arena MUST be
    // declared first so it outlives the TLSF (reverse destruction order).
    crd::memory::VirtualMemoryAllocator::Config vmcfg;
    vmcfg.reserve_bytes = crd::usize{512} << 20; // 512 MiB address space
    crd::memory::VirtualMemoryAllocator vm(vmcfg);

    // Small nominal chunk so a handful of allocations forces cross-chunk growth
    // (and keeps VM physical commit modest — each grow commits chunk_bytes).
    const crd::usize      chunk = crd::memory::TlsfAllocator::min_pool_size() + (64U * 1024U);
    GrowableTlsfAllocator a(chunk, &vm); // parent = the VM arena, not malloc

    REQUIRE(vm.used_bytes() == 0); // nothing pulled until the first allocation

    crd::containers::Array<void*> ptrs(crd::memory::default_allocator()); // bookkeeping only
    for (int i = 0; i < 64; ++i)
    {
        void* p = a.allocate(16U * 1024U, 16);
        REQUIRE(p != nullptr);
        REQUIRE(a.owns(p));
        REQUIRE(vm.owns(p)); // the block physically lives inside the VM reservation
        std::memset(p, i & 0xFF, 16U * 1024U); // ASan: in-bounds within committed VM pages
        ptrs.push_back(p);
    }
    REQUIRE(a.num_chunks() > 1);     // genuinely grew across chunks
    REQUIRE(vm.used_bytes() > 0);    // chunks were pulled from the VM arena, not malloc
    REQUIRE(vm.committed_bytes() >= vm.used_bytes());

    // Free everything, then reuse — the freed blocks return to their TLSF pools
    // (the VM arena itself is grow-mostly: it does not reclaim chunk address space,
    // which is fine for this long-lived-heap role).
    for (void* p : ptrs) { a.deallocate(p); }
    void* reused = a.allocate(16U * 1024U, 16);
    REQUIRE(reused != nullptr);
    REQUIRE(vm.owns(reused));
    a.deallocate(reused);
}

TEST_CASE("GrowableTlsfAllocator try_allocate is graceful when a VM parent is exhausted (ADR-0085 S3)",
          "[memory][growable-tlsf][vmalloc]")
{
    // grow() pulls each chunk from the parent via try_allocate, so exhausting a
    // small VM reservation must yield nullptr end-to-end — NOT a CRD_FATAL.
    crd::memory::VirtualMemoryAllocator::Config vmcfg;
    vmcfg.reserve_bytes = crd::usize{8} << 20; // 8 MiB total backing
    crd::memory::VirtualMemoryAllocator vm(vmcfg);

    GrowableTlsfAllocator a(crd::usize{2} << 20, &vm); // 2 MiB chunks

    bool             hit_null = false;
    crd::containers::Array<void*> ptrs(crd::memory::default_allocator());
    for (int i = 0; i < 32; ++i) // bounded: ~4 chunks fit in 8 MiB, then exhaustion
    {
        void* p = a.try_allocate(crd::usize{1} << 20, 16); // 1 MiB asks
        if (p == nullptr)
        {
            hit_null = true; // graceful exhaustion — the whole point of the fix
            break;
        }
        REQUIRE(vm.owns(p));
        ptrs.push_back(p);
    }
    REQUIRE(hit_null); // reached graceful nullptr without aborting
    REQUIRE(ptrs.size() > 0); // but some allocations did succeed first
    for (void* p : ptrs) { a.deallocate(p); }
}

TEST_CASE("GrowableTlsfAllocator deallocate dispatches to the owning chunk", "[memory][growable-tlsf]")
{
    const crd::usize      chunk = crd::memory::TlsfAllocator::min_pool_size() + (32U * 1024U);
    GrowableTlsfAllocator a(chunk);
    void*                 p0 = a.allocate(20U * 1024U, 16); // forces chunk #1
    void*                 p1 = a.allocate(20U * 1024U, 16); // forces chunk #2
    REQUIRE(a.num_chunks() >= 2);
    REQUIRE(a.allocation_size(p0) >= 20U * 1024U);
    a.deallocate(p0); // must find p0's chunk, not p1's
    a.deallocate(p1);
    // Reuse after free succeeds (the freed blocks are back in their pools).
    void* p2 = a.allocate(20U * 1024U, 16);
    REQUIRE(p2 != nullptr);
    a.deallocate(p2);
}
