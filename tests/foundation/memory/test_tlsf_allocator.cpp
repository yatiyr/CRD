#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <random>

using crd::memory::IAllocator;
using crd::memory::TlsfAllocator;

namespace
{
constexpr crd::usize kPoolMB = 1024U * 1024U; // 1 MB pool — comfortable for tests

bool is_aligned(const void* p, crd::usize alignment)
{
    return (reinterpret_cast<std::uintptr_t>(p) & (alignment - 1)) == 0;
}
} // namespace

TEST_CASE("TLSF construction with owning ctor allocates from parent", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};
    CHECK(a.pool_capacity() == kPoolMB);
    CHECK(a.pool_base() != nullptr);
}

TEST_CASE("TLSF construction with non-owning ctor uses pre-allocated buffer", "[memory][tlsf]")
{
    alignas(16) static unsigned char buffer[kPoolMB];
    TlsfAllocator a{buffer, kPoolMB};
    CHECK(a.pool_capacity() == kPoolMB);
    CHECK(a.pool_base() != nullptr);
}

TEST_CASE("TLSF allocate returns 16-byte aligned pointer by default", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};
    void* p = a.allocate(64);
    REQUIRE(p != nullptr);
    CHECK(is_aligned(p, 16U));
    a.deallocate(p);
}

TEST_CASE("TLSF allocate honours explicit alignment up to 256", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};

    void* p16 = a.allocate(100, 16);
    void* p32 = a.allocate(100, 32);
    void* p64 = a.allocate(100, 64);
    void* p128 = a.allocate(100, 128);
    void* p256 = a.allocate(100, 256);

    REQUIRE(p16 != nullptr);
    REQUIRE(p32 != nullptr);
    REQUIRE(p64 != nullptr);
    REQUIRE(p128 != nullptr);
    REQUIRE(p256 != nullptr);
    CHECK(is_aligned(p16, 16U));
    CHECK(is_aligned(p32, 32U));
    CHECK(is_aligned(p64, 64U));
    CHECK(is_aligned(p128, 128U));
    CHECK(is_aligned(p256, 256U));

    a.deallocate(p16);
    a.deallocate(p32);
    a.deallocate(p64);
    a.deallocate(p128);
    a.deallocate(p256);
}

TEST_CASE("TLSF aligned alloc-dealloc round-trip preserves coalesce", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};

    // Hammer the alignment-split path then drain. After draining the pool
    // must coalesce back to a single big free block.
    crd::containers::Array<void*> live;
    for (int i = 0; i < 50; ++i)
    {
        void* p = a.allocate(96, 64);
        REQUIRE(p != nullptr);
        REQUIRE(is_aligned(p, 64U));
        live.push_back(p);
    }
    for (void* p : live)
    {
        a.deallocate(p);
    }
    void* big = a.allocate(kPoolMB / 4U);
    CHECK(big != nullptr);
    a.deallocate(big);
}

TEST_CASE("TLSF deallocate(nullptr) is a no-op", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};
    a.deallocate(nullptr);
    SUCCEED();
}

TEST_CASE("TLSF allocate/deallocate round-trip preserves capacity", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};

    // Fill with many small allocations, then free them all. The pool must
    // coalesce back to a single big free block, so a subsequent large
    // allocation succeeds.
    crd::containers::Array<void*> ptrs;
    for (int i = 0; i < 100; ++i)
    {
        void* p = a.allocate(256);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }

    for (void* p : ptrs)
    {
        a.deallocate(p);
    }

    // Now allocate a big block — must succeed if coalesce is working.
    void* big = a.allocate(kPoolMB / 2U);
    CHECK(big != nullptr);
    a.deallocate(big);
}

TEST_CASE("TLSF coalesces with previous and next free neighbours", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};

    void* a_p = a.allocate(1024);
    void* b_p = a.allocate(1024);
    void* c_p = a.allocate(1024);
    REQUIRE(a_p != nullptr);
    REQUIRE(b_p != nullptr);
    REQUIRE(c_p != nullptr);

    // Free middle first — leaves [used, free, used].
    a.deallocate(b_p);
    // Free first — coalesces forward into the gap. Now [free, used].
    a.deallocate(a_p);
    // Free last — coalesces backward. Pool should be one big free block.
    a.deallocate(c_p);

    // A single large allocation must succeed (within TLSF's rounded-up search class).
    void* big = a.allocate(kPoolMB / 2U);
    CHECK(big != nullptr);
    a.deallocate(big);
}

TEST_CASE("TLSF allocation_size returns the actual block size", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};
    void* p = a.allocate(100);
    REQUIRE(p != nullptr);

    const crd::usize sz = a.allocation_size(p);
    CHECK(sz >= 100U);
    CHECK(sz % 16U == 0U); // rounded to alignment

    a.deallocate(p);
}

TEST_CASE("TLSF owns returns true for allocated pointers, false otherwise", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};
    void* p = a.allocate(64);
    REQUIRE(p != nullptr);

    CHECK(a.owns(p));
    CHECK_FALSE(a.owns(nullptr));

    int stack_var = 0;
    CHECK_FALSE(a.owns(&stack_var));

    a.deallocate(p);
}

TEST_CASE("TLSF reallocate grows in place when next block is free", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};

    void* p = a.allocate(256);
    REQUIRE(p != nullptr);
    std::memset(p, 0xAB, 256);

    void* q = a.reallocate(p, 256, 1024);
    REQUIRE(q != nullptr);
    // First 256 bytes should be preserved (in-place or copied).
    auto* bytes = static_cast<unsigned char*>(q);
    for (int i = 0; i < 256; ++i)
    {
        CHECK(bytes[i] == 0xAB);
    }
    a.deallocate(q);
}

TEST_CASE("TLSF reallocate shrinks in place", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};

    void* p = a.allocate(2048);
    REQUIRE(p != nullptr);
    std::memset(p, 0xCD, 100);

    void* q = a.reallocate(p, 2048, 256);
    REQUIRE(q != nullptr);
    // Same address (shrink in place).
    CHECK(q == p);
    auto* bytes = static_cast<unsigned char*>(q);
    for (int i = 0; i < 100; ++i)
    {
        CHECK(bytes[i] == 0xCD);
    }
    a.deallocate(q);
}

TEST_CASE("TLSF reallocate(nullptr, ...) is allocate", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};
    void* p = a.reallocate(nullptr, 0, 256);
    REQUIRE(p != nullptr);
    CHECK(a.owns(p));
    a.deallocate(p);
}

TEST_CASE("TLSF reallocate to size 0 is deallocate", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};
    void* p = a.allocate(256);
    REQUIRE(p != nullptr);
    void* q = a.reallocate(p, 256, 0);
    CHECK(q == nullptr);
}

TEST_CASE("TLSF allocate then free then allocate same size returns usable block", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};

    void* p1 = a.allocate(512);
    REQUIRE(p1 != nullptr);
    a.deallocate(p1);

    void* p2 = a.allocate(512);
    REQUIRE(p2 != nullptr);
    CHECK(a.owns(p2));
    a.deallocate(p2);
}

TEST_CASE("TLSF aligned allocations are usable end-to-end", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};

    void* p = a.allocate(200, 16);
    REQUIRE(p != nullptr);
    REQUIRE(is_aligned(p, 16U));

    // Write the full 200 bytes — no overlap with header / neighbour blocks.
    std::memset(p, 0xEE, 200);

    auto* bytes = static_cast<unsigned char*>(p);
    for (int i = 0; i < 200; ++i)
    {
        CHECK(bytes[i] == 0xEE);
    }

    a.deallocate(p);
}

TEST_CASE("TLSF many small allocations across SL classes do not corrupt headers", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};

    crd::containers::Array<void*> ptrs;
    for (int i = 0; i < 50; ++i)
    {
        const crd::usize sz = 16U + (static_cast<crd::usize>(i) * 16U);
        void* p = a.allocate(sz);
        REQUIRE(p != nullptr);
        std::memset(p, i & 0xFF, sz);
        ptrs.push_back(p);
    }
    // All should still resolve to valid memory.
    for (crd::usize i = 0; i < ptrs.size(); ++i)
    {
        CHECK(a.owns(ptrs[i]));
    }
    for (void* p : ptrs)
    {
        a.deallocate(p);
    }

    void* big = a.allocate(kPoolMB / 4U);
    CHECK(big != nullptr);
    a.deallocate(big);
}

TEST_CASE("TLSF stress: 1000 random alloc/free with mixed alignments", "[memory][tlsf][stress]")
{
    TlsfAllocator a{4U * kPoolMB}; // 4 MB pool for headroom

    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng{12345};
    crd::containers::Array<void*> live;
    std::uniform_int_distribution<int> op_pick(0, 2);
    std::uniform_int_distribution<crd::usize> size_pick(16U, 4096U);
    std::uniform_int_distribution<int> align_pick(0, 4); // 16/32/64/128/256
    constexpr crd::usize align_choices[] = {16U, 32U, 64U, 128U, 256U};

    for (int iter = 0; iter < 1000; ++iter)
    {
        const bool do_free = (live.size() > 0) && (op_pick(rng) == 0);
        if (do_free)
        {
            std::uniform_int_distribution<crd::usize> idx_pick(0U, live.size() - 1U);
            const crd::usize idx = idx_pick(rng);
            a.deallocate(live[idx]);
            live.swap_remove(idx);
        }
        else
        {
            const crd::usize sz = size_pick(rng);
            const crd::usize al = align_choices[align_pick(rng)];
            void* p = a.allocate(sz, al);
            REQUIRE(p != nullptr);
            REQUIRE(is_aligned(p, al));
            std::memset(p, iter & 0xFF, sz);
            live.push_back(p);
        }
    }

    // Drain.
    for (void* p : live)
    {
        a.deallocate(p);
    }

    // Pool must coalesce back so a 2 MB allocation succeeds (well within TLSF's
    // rounded-up search class for a 4 MB pool).
    void* big = a.allocate(2U * kPoolMB);
    CHECK(big != nullptr);
    a.deallocate(big);
}

TEST_CASE("TLSF reports default_allocator-compatible IAllocator interface", "[memory][tlsf]")
{
    TlsfAllocator a{kPoolMB};

    IAllocator& iface = a;
    void* p = iface.allocate(128, 32);
    REQUIRE(p != nullptr);
    CHECK(is_aligned(p, 32U));
    CHECK(iface.owns(p));
    iface.deallocate(p);
}

TEST_CASE("TLSF try_allocate returns nullptr on out-of-memory", "[memory][tlsf][try]")
{
    // Smallest pool that still allows construction.
    const crd::usize cap = TlsfAllocator::min_pool_size() + 256U;
    TlsfAllocator a{cap};

    void* p = a.try_allocate(64);
    REQUIRE(p != nullptr);

    // Should fail (request larger than the pool's largest free class).
    void* q = a.try_allocate(cap * 2U);
    CHECK(q == nullptr);

    a.deallocate(p);
}

TEST_CASE("TLSF try_allocate(0) returns nullptr", "[memory][tlsf][try]")
{
    TlsfAllocator a{kPoolMB};
    CHECK(a.try_allocate(0) == nullptr);
}

// BOUNDARY-ADVERSARY regression for the init_pool end-sentinel bug (the multifrontal-LU flaky-AV root cause,
// fixed 2026-06-09; see docs/SANITY.md "Boundary adversaries, not volume"). init_pool placed the end sentinel
// 16 B BEFORE block_next(free_block), so a block that EXACTLY reached the pool's TAIL read uninitialised slack
// as its "next" block; if that slack's low bit looked like kFreeBit, freeing the tail block coalesced garbage
// and smashed the free list (list_remove on junk links → AV). It was benign until a block fit the pool to its
// last byte, so 820 K+ random-stress assertions never tripped it — random alloc/free almost never produces the
// exact-fit-to-tail block, and uniform fill leaves a free remainder that shields the tail.
//
// This test is the DECISIVE trigger: it PRE-POISONS the buffer with 0xCD (low bit set ⇒ "looks free") before
// construction, then allocates the LARGEST single block the pool can serve — which by construction spans to the
// tail, so block_next(block) == the end sentinel (the exact computation the bug got wrong) — touches every byte
// (ASan flags a write past the buffer edge), and FREES it, repeatedly. With the buggy placement this AVs/smashes
// on the first free; with the fix block_next lands on the size-0 end sentinel (block_is_last ⇒ no coalesce) and
// every round is clean. Verified to FAIL on the buggy one-liner and PASS on the fix (docs/SANITY.md rule #2/#3).
TEST_CASE("TLSF exact-fit-to-tail alloc/free does not corrupt the free list (init_pool end-sentinel regression)",
          "[memory][tlsf][boundary][regression]")
{
    constexpr crd::usize cap = 64U * 1024U;
    alignas(16) static crd::u8 buffer[cap];
    std::memset(buffer, 0xCD, cap); // poison the tail slack BEFORE the allocator overwrites it with sentinels

    TlsfAllocator a{buffer, cap};

    // Largest single block the pool can serve right now (each probe allocates then frees ⇒ state restored).
    auto largest_serviceable = [&a]() -> crd::usize // `cap` is constexpr ⇒ usable without capture (clang-cl -Wunused-lambda-capture)
    {
        crd::usize lo = 0U;
        crd::usize hi = cap;
        while (lo < hi)
        {
            const crd::usize mid = lo + (hi - lo + 1U) / 2U;
            void* p = a.try_allocate(mid);
            if (p != nullptr)
            {
                a.deallocate(p);
                lo = mid;
            }
            else
            {
                hi = mid - 1U;
            }
        }
        return lo;
    };

    // Fill the pool to exhaustion by always taking the LARGEST serviceable block. TLSF rounds large requests up
    // by ~1 KB, so a big request always SPLITS and leaves a free remainder shielding the tail — that is exactly
    // why a "largest single block" test (and uniform fill) does NOT bite. But the remainder shrinks each round;
    // once it drops below the small-block boundary (512 B) the serviceable-max allocation NO-SPLITS and consumes
    // the free block to its last byte, so block_next(that block) == the end sentinel. The LAST allocated block is
    // therefore exact-fit-to-tail — the precise edge the init_pool bug got wrong.
    crd::containers::Array<void*> live;
    for (int guard = 0; guard < 100000; ++guard)
    {
        const crd::usize sz = largest_serviceable();
        if (sz == 0U) { break; } // pool exhausted
        void* p = a.try_allocate(sz);
        if (p == nullptr) { break; }
        CHECK(a.owns(p));
        std::memset(p, 0xAB, sz); // touch every byte ⇒ ASan catches a write over the buffer edge
        live.push_back(p);
    }
    REQUIRE(live.size() > 0U);

    // Free the TAIL-MOST block first: its block_next is the end sentinel. With the buggy placement that is
    // uninitialised 0xCD slack whose low bit looks like kFreeBit, so deallocate coalesces garbage and smashes the
    // free list (OOB free-list index / AV). With the fix it is the real size-0 sentinel (block_is_last ⇒ no
    // coalesce) and the free survives. Drain the rest tail-first too, then prove the list still serves a refill.
    while (live.size() > 0U)
    {
        a.deallocate(live[live.size() - 1U]);
        live.swap_remove(live.size() - 1U);
    }
    void* refill = a.try_allocate(largest_serviceable());
    REQUIRE(refill != nullptr); // a smashed list would fail or AV here
    a.deallocate(refill);
}
