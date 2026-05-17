// Stress coverage for crd-memory allocators (detour D-002, slice v0 — harness
// shakedown; the deep adversarial fragmentation/coalescing matrix is v5).
//
// Allocators are single-threaded-by-contract, so the concurrency story is
// *isolation*, not sharing: N workers each pound their OWN TlsfAllocator with a
// randomised alloc / free / realloc mix, every live block stamped with a
// per-block byte pattern that is re-verified on every touch. Any overlap of two
// "live" blocks (a coalescing/split bug, a header smash) shows up as a pattern
// mismatch — or ASan catches the OOB directly. The seed prints on failure.

#include "stress_harness.hpp"

#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

struct Block
{
    u8* ptr = nullptr;
    usize size = 0;
    u8 fill = 0;
};

void stamp(Block& b) noexcept
{
    std::memset(b.ptr, b.fill, b.size);
}

// Cheap-but-effective pattern check: first byte, last byte, and a probe in the
// middle. Full memcmp would dominate runtime; this catches header smashes and
// neighbour-overlap on the boundaries that matter.
bool verify(const Block& b) noexcept
{
    if (b.size == 0)
    {
        return true;
    }
    if (b.ptr[0] != b.fill || b.ptr[b.size - 1] != b.fill)
    {
        return false;
    }
    return b.ptr[b.size / 2] == b.fill;
}

void drive_tlsf(crd::stress::Config cfg)
{
    crd::stress::FailSink sink;

    const auto work = [&sink](u32 w, u64 iters, crd::stress::Rng& rng)
    {
        // Per-worker parent + heap — zero sharing; allocators are single-threaded-by-contract.
        crd::memory::MallocAllocator parent("stress-alloc-parent");
        crd::memory::TlsfAllocator heap(usize{4} << 20, &parent, "stress-alloc-heap");

        constexpr u32 kMaxLive = 96U;
        Block live[kMaxLive] = {};
        u32 live_count = 0;
        u8 fill_seq = static_cast<u8>(w * 7U + 1U);

        const auto free_at = [&](u32 idx)
        {
            CRD_STRESS_FAIL_IF(sink, w, 0, verify(live[idx]), "block corrupted before free");
            heap.deallocate(live[idx].ptr);
            live[idx] = live[--live_count]; // swap-remove
        };

        for (u64 it = 0; it < iters; ++it)
        {
            const u32 roll = rng.next_u32(10U);

            if ((roll < 6U && live_count < kMaxLive)) // 60%: allocate (skewed toward small)
            {
                const u32 bucket = rng.next_u32(4U);
                usize size = 1U + rng.next_u32(64U * 1024U);
                if (bucket == 0U)      { size = 1U + rng.next_u32(64U); }
                else if (bucket == 1U) { size = 1U + rng.next_u32(512U); }
                else if (bucket == 2U) { size = 1U + rng.next_u32(8U * 1024U); }
                const usize align = usize{8} << rng.next_u32(6U); // 8..256
                void* p = heap.try_allocate(size, align);
                if (p == nullptr) // pool pressure — free something and move on
                {
                    if (live_count > 0U)
                    {
                        free_at(rng.next_u32(live_count));
                    }
                    continue;
                }
                CRD_STRESS_FAIL_IF(sink, w, it, (reinterpret_cast<usize>(p) & (align - 1U)) == 0U,
                                   "try_allocate returned a mis-aligned pointer");
                CRD_STRESS_FAIL_IF(sink, w, it, heap.owns(p), "heap does not own a pointer it just returned");
                Block b{static_cast<u8*>(p), size, fill_seq++};
                if (fill_seq == 0U)
                {
                    fill_seq = 1U;
                } // keep fills non-zero so a fresh 0-page is distinguishable
                stamp(b);
                live[live_count++] = b;
            }
            else if (roll < 8U && live_count > 0U) // 20%: free
            {
                free_at(rng.next_u32(live_count));
            }
            else if (live_count > 0U) // 20%: reallocate (preserve-content path)
            {
                const u32 idx = rng.next_u32(live_count);
                Block& b = live[idx];
                CRD_STRESS_FAIL_IF(sink, w, it, verify(b), "block corrupted before realloc");
                const usize new_size = 1U + rng.next_u32(32U * 1024U);
                u8* np = static_cast<u8*>(heap.reallocate(b.ptr, b.size, new_size));
                CRD_STRESS_FAIL_IF(sink, w, it, np != nullptr, "reallocate returned null");
                if (np == nullptr)
                {
                    continue;
                }
                const usize keep = b.size < new_size ? b.size : new_size;
                // Surviving prefix must still hold the old fill.
                bool prefix_ok = true;
                if (keep > 0)
                {
                    prefix_ok = (np[0] == b.fill) && (np[keep - 1] == b.fill);
                }
                CRD_STRESS_FAIL_IF(sink, w, it, prefix_ok, "reallocate did not preserve contents");
                b.ptr = np;
                b.size = new_size;
                stamp(b); // re-stamp whole (possibly larger) region with the same fill
            }
        }

        // Drain — frees every live block, each verified one last time.
        while (live_count > 0U)
        {
            free_at(live_count - 1U);
        }
    };

    const auto oracle = [](u32 /*round*/) { /* per-worker heaps; nothing global */ };

    crd::stress::run(cfg, work, oracle);
    CRD_STRESS_ORACLE_OK(sink);
}
} // namespace

TEST_CASE("allocators stress -- isolated TlsfAllocator alloc/free/realloc churn", "[stress][memory]")
{
    SECTION("fibers")
    {
        drive_tlsf(crd::stress::bounded(crd::stress::RunMode::Fibers));
    }
    SECTION("threads")
    {
        drive_tlsf(crd::stress::bounded(crd::stress::RunMode::Threads));
    }
}

TEST_CASE("allocators stress -- isolated TlsfAllocator churn (soak)", "[stress][memory][.soak]")
{
    drive_tlsf(crd::stress::soak(crd::stress::RunMode::Fibers));
}
