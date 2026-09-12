// test_ring_allocator.cpp — crd::memory::RingAllocator (ADR-0085 S4).
//
// Epoch/fence-gated FIFO staging: basic claim + alignment, oversized/zero ->
// nullptr, fill-to-full -> retire -> reclaim, wraparound padding, epoch reclaim,
// K-epoch recycling over many epochs, a VM-backed (malloc-free) staging arena,
// and the multi-producer race that proves the lock-free claim hands out disjoint
// regions (ASan-clean).

#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/ring_allocator.hpp>
#include <crd/memory/allocators/virtual_memory_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

using crd::memory::RingAllocator;

namespace
{
[[nodiscard]] bool is_aligned(const void* p, crd::usize a) noexcept
{
    return (reinterpret_cast<std::uintptr_t>(p) & (a - 1)) == 0;
}
} // namespace

TEST_CASE("RingAllocator basic claim is aligned and grows in_use", "[memory][ring]")
{
    RingAllocator ring(64U * 1024U);
    REQUIRE(ring.capacity() == 64U * 1024U);
    REQUIRE(ring.in_use_bytes() == 0);

    void* p = ring.try_claim(100, 16);
    REQUIRE(p != nullptr);
    REQUIRE(is_aligned(p, 16));
    REQUIRE(ring.in_use_bytes() >= 100);
    std::memset(p, 0xAB, 100); // ASan: in-bounds
}

TEST_CASE("RingAllocator rejects zero and oversized claims", "[memory][ring]")
{
    RingAllocator ring(4096);
    REQUIRE(ring.try_claim(0) == nullptr);
    REQUIRE(ring.try_claim(4097) == nullptr);     // larger than the whole ring
    REQUIRE(ring.try_claim(1U << 30) == nullptr); // way larger
    REQUIRE(ring.try_claim(4096) != nullptr);     // exactly fits
}

TEST_CASE("RingAllocator fills, refuses, then reclaims after retire", "[memory][ring]")
{
    RingAllocator ring(4096);
    // Epoch 0 claims fill the ring.
    REQUIRE(ring.try_claim(2048, 16) != nullptr);
    REQUIRE(ring.try_claim(2048, 16) != nullptr);
    REQUIRE(ring.try_claim(16, 16) == nullptr); // full

    ring.begin_epoch(1);   // close epoch 0
    ring.retire(0);        // epoch 0 done -> reclaim its 4096 bytes
    REQUIRE(ring.in_use_bytes() == 0);
    REQUIRE(ring.try_claim(2048, 16) != nullptr); // space available again
}

TEST_CASE("RingAllocator wraps to offset 0 when a span straddles the end", "[memory][ring]")
{
    RingAllocator ring(4096);
    void* x = ring.try_claim(3072, 16); // epoch 0, offset 0..3072 (will be retired)
    REQUIRE(x != nullptr);

    ring.begin_epoch(1);
    void* a = ring.try_claim(512, 16); // epoch 1, offset 3072..3584 — stays LIVE
    REQUIRE(a != nullptr);
    REQUIRE(a > x);

    ring.begin_epoch(2);
    ring.retire(0); // free epoch 0 -> offset 0..3072 is reclaimable

    // 1024 won't fit in the remaining 512 at the tail -> wraps to offset 0, which is
    // now free; `a` (offset 3072) is still live and untouched.
    void* b = ring.try_claim(1024, 16);
    REQUIRE(b != nullptr);
    REQUIRE(b < a);                                          // wrapped to the front, below the live `a`
    REQUIRE(reinterpret_cast<std::uintptr_t>(b) + 1024 <= reinterpret_cast<std::uintptr_t>(a)); // disjoint from `a`
    std::memset(b, 0xCD, 1024); // ASan: the wrapped region is in-bounds
}

TEST_CASE("RingAllocator recycles across many epochs without exhausting K", "[memory][ring]")
{
    RingAllocator ring(8192, nullptr, 4); // K = 4 epochs in flight
    // Drive far more than K epochs; retiring as we go must keep it healthy.
    for (crd::u64 e = 1; e <= 100; ++e)
    {
        void* p = ring.try_claim(2048, 16);
        REQUIRE(p != nullptr);
        std::memset(p, static_cast<int>(e & 0xFF), 2048);
        ring.begin_epoch(e);   // close the previous epoch
        ring.retire(e - 1);    // retire it immediately
    }
    REQUIRE(ring.current_epoch() == 100);
}

TEST_CASE("RingAllocator over a VirtualMemoryAllocator is a malloc-free staging arena", "[memory][ring][vmalloc]")
{
    crd::memory::VirtualMemoryAllocator::Config vmcfg;
    vmcfg.reserve_bytes = crd::usize{64} << 20;
    crd::memory::VirtualMemoryAllocator vm(vmcfg);

    RingAllocator ring(crd::usize{1} << 20, &vm); // 1 MiB staging from the VM arena
    REQUIRE(vm.used_bytes() >= (crd::usize{1} << 20)); // buffer committed from VM, not malloc

    void* p = ring.try_claim(4096, 64);
    REQUIRE(p != nullptr);
    REQUIRE(vm.owns(p)); // the staging buffer physically lives in the VM reservation
    std::memset(p, 0x5A, 4096);
}

namespace
{
constexpr crd::u32   kTotalClaims = 32000;
constexpr crd::usize kBlock       = 48; // unaligned size -> exercises alignment padding

struct Rec
{
    std::uintptr_t addr;
    crd::usize     size;
};

// Captured by value into the parallel_for job (one pointer-sized context — keeps
// the Task within the jobs SBO budget). Catch2 REQUIRE is NOT thread-safe, so the
// jobs only record; the main thread asserts after the wait().
struct ClaimCtx
{
    RingAllocator*           ring;
    Rec*                     recs;
    std::atomic<crd::usize>* count;
    std::atomic<bool>*       any_null;
};
} // namespace

TEST_CASE("RingAllocator concurrent producers claim strictly disjoint regions", "[memory][ring]")
{
    // Lock-free claim correctness, driven by the engine's fiber job system (not raw
    // std::thread): many producers claim simultaneously and no two live regions may
    // overlap. Sized so the ring never wraps or needs retire, so every claim is a
    // distinct disjoint span we can verify after the fact.
    const crd::usize cap = static_cast<crd::usize>(kTotalClaims) * (kBlock + 16) + 4096;
    RingAllocator    ring(cap);

    crd::containers::Array<Rec> recs(crd::memory::default_allocator());
    recs.resize(kTotalClaims, Rec{0, 0});
    std::atomic<crd::usize> count{0};
    std::atomic<bool>       any_null{false};

    crd::jobs::init();
    {
        ClaimCtx ctx{&ring, recs.begin(), &count, &any_null};
        crd::jobs::Counter* c = crd::jobs::parallel_for(kTotalClaims, crd::jobs::num_workers(),
                                                        [ctx](crd::u32 begin, crd::u32 end) {
                                                            for (crd::u32 i = begin; i < end; ++i)
                                                            {
                                                                void* p = ctx.ring->try_claim(kBlock, 16);
                                                                if (p == nullptr)
                                                                {
                                                                    ctx.any_null->store(true, std::memory_order_relaxed);
                                                                    continue;
                                                                }
                                                                std::memset(p, static_cast<int>(i & 0xFF), kBlock);
                                                                const crd::usize idx =
                                                                    ctx.count->fetch_add(1, std::memory_order_relaxed);
                                                                ctx.recs[idx] =
                                                                    Rec{reinterpret_cast<std::uintptr_t>(p), kBlock};
                                                            }
                                                        });
        crd::jobs::wait(c);
    }
    crd::jobs::shutdown();

    REQUIRE_FALSE(any_null.load()); // ring never spuriously refused (sized to fit)
    const crd::usize n = count.load();
    REQUIRE(n == kTotalClaims);

    // Sort by address and assert no two claimed regions overlap.
    std::sort(recs.begin(), recs.begin() + static_cast<std::ptrdiff_t>(n),
              [](const Rec& a, const Rec& b) { return a.addr < b.addr; });
    for (crd::usize i = 1; i < n; ++i)
    {
        REQUIRE(recs[i - 1].addr + recs[i - 1].size <= recs[i].addr); // disjoint
    }
}
