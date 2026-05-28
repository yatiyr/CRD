// test_streaming_allocator.cpp — crd::memory::StreamingAllocator (ADR-0085 S5).
//
// The streaming policy layer: per-category budgets gate resident allocation, an
// injected IResidencyPolicy sheds under pressure, the resident store (GrowableTlsf
// over a VM arena) gives real per-payload free/reuse, and staging forwards to the
// lock-free RingAllocator. Covers budget gating, null-policy graceful failure, the
// pressure protocol + custom policy invocation, multi-category isolation, staging
// composition, and stable-address per-payload reuse.

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/streaming_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using crd::memory::CategoryId;
using crd::memory::IResidencyPolicy;
using crd::memory::StreamingAllocator;

namespace
{
[[nodiscard]] StreamingAllocator::Config small_cfg() noexcept
{
    StreamingAllocator::Config c;
    c.reserve_bytes        = crd::usize{256} << 20; // 256 MiB address space
    c.resident_chunk_bytes = crd::usize{4} << 20;   // 4 MiB resident chunks
    c.staging_bytes        = crd::usize{1} << 20;   // 1 MiB staging
    c.staging_epochs       = 4;
    c.num_categories       = 4;
    return c;
}

// A FIFO eviction policy for the pressure tests: evicts the oldest live allocations
// of the requested category (calling back into release_resident) until it has freed
// at least `needed` bytes. Returns 0 when it has nothing left to shed.
class FifoEvictPolicy final : public IResidencyPolicy
{
public:
    explicit FifoEvictPolicy(crd::memory::IAllocator* a) : m_recs(a) {}

    void track(CategoryId cat, void* p, crd::u64 size) { m_recs.push_back(Rec{cat, p, size, true}); }
    [[nodiscard]] crd::usize evict_calls() const noexcept { return m_evict_calls; }

    [[nodiscard]] crd::u64 evict(StreamingAllocator& sa, CategoryId category, crd::u64 needed) override
    {
        ++m_evict_calls;
        crd::u64 freed = 0;
        for (crd::usize i = 0; i < m_recs.size() && freed < needed; ++i)
        {
            if (m_recs[i].live && m_recs[i].cat == category)
            {
                sa.release_resident(category, m_recs[i].p);
                freed += m_recs[i].size;
                m_recs[i].live = false;
            }
        }
        return freed;
    }

private:
    struct Rec
    {
        CategoryId cat;
        void*      p;
        crd::u64   size;
        bool       live;
    };
    crd::containers::Array<Rec> m_recs;
    crd::usize                  m_evict_calls = 0;
};
} // namespace

TEST_CASE("StreamingAllocator resident allocate charges and release un-charges", "[memory][streaming]")
{
    StreamingAllocator sa(small_cfg());
    REQUIRE(sa.used(0) == 0);

    void* p = sa.try_allocate_resident(0, 64 * 1024, 64);
    REQUIRE(p != nullptr);
    REQUIRE(sa.used(0) >= 64 * 1024);    // charged the actual block size
    REQUIRE(sa.resident_committed_bytes() > 0); // backed by committed VM, not malloc
    std::memset(p, 0xAB, 64 * 1024);     // ASan: in-bounds

    const crd::u64 charged = sa.used(0);
    sa.release_resident(0, p);
    REQUIRE(sa.used(0) == 0);            // fully un-charged
    REQUIRE(charged >= 64 * 1024);
}

TEST_CASE("StreamingAllocator enforces a per-category budget with the null policy", "[memory][streaming]")
{
    StreamingAllocator sa(small_cfg()); // default policy = NullResidencyPolicy (sheds nothing)
    sa.set_budget(0, 100 * 1024);
    REQUIRE(sa.budget(0) == 100 * 1024);

    crd::usize succeeded = 0;
    for (int i = 0; i < 50; ++i)
    {
        void* p = sa.try_allocate_resident(0, 10 * 1024, 16);
        if (p == nullptr) { break; } // budget reached; null policy can't shed -> graceful nullptr
        ++succeeded;
    }
    REQUIRE(succeeded >= 1);                  // some fit
    REQUIRE(succeeded < 50);                  // but not all — the budget bit
    REQUIRE(sa.used(0) <= sa.budget(0));      // budget was never exceeded
}

TEST_CASE("StreamingAllocator pressure protocol invokes the injected policy", "[memory][streaming]")
{
    FifoEvictPolicy    policy(crd::memory::default_allocator());
    StreamingAllocator sa(small_cfg(), &policy);
    sa.set_budget(0, 100 * 1024); // room for ~10 x 10 KiB at once

    // Allocate far more than the budget; the policy must evict oldest to make room,
    // so every allocation succeeds and the budget is respected throughout.
    for (int i = 0; i < 40; ++i)
    {
        void* p = sa.try_allocate_resident(0, 10 * 1024, 16);
        REQUIRE(p != nullptr); // policy kept making room
        policy.track(0, p, 10 * 1024);
        std::memset(p, i & 0xFF, 10 * 1024);
        REQUIRE(sa.used(0) <= sa.budget(0) + 10 * 1024); // budget held (within one block's slack)
    }
    REQUIRE(policy.evict_calls() > 0); // pressure actually engaged the policy
}

TEST_CASE("StreamingAllocator null policy fails gracefully (no fatal) when full", "[memory][streaming]")
{
    StreamingAllocator sa(small_cfg());
    sa.set_budget(1, 8 * 1024);
    REQUIRE(sa.try_allocate_resident(1, 8 * 1024, 16) != nullptr); // fills the budget
    REQUIRE(sa.try_allocate_resident(1, 8 * 1024, 16) == nullptr); // over budget -> nullptr, not abort
}

TEST_CASE("StreamingAllocator budgets are per-category isolated", "[memory][streaming]")
{
    StreamingAllocator sa(small_cfg());
    sa.set_budget(0, 16 * 1024); // category 0 tightly bounded
    // category 1 left at the default (unlimited)

    // Exhaust category 0.
    REQUIRE(sa.try_allocate_resident(0, 16 * 1024, 16) != nullptr);
    REQUIRE(sa.try_allocate_resident(0, 16 * 1024, 16) == nullptr); // cat 0 full

    // category 1 is unaffected by category 0 being over budget.
    void* p = sa.try_allocate_resident(1, 256 * 1024, 16);
    REQUIRE(p != nullptr);
    REQUIRE(sa.used(1) >= 256 * 1024);
    REQUIRE(sa.used(0) <= sa.budget(0));
}

TEST_CASE("StreamingAllocator resident free enables stable-address reuse", "[memory][streaming]")
{
    StreamingAllocator sa(small_cfg());
    void* a = sa.try_allocate_resident(2, 32 * 1024, 16);
    REQUIRE(a != nullptr);
    sa.release_resident(2, a);
    void* b = sa.try_allocate_resident(2, 32 * 1024, 16);
    REQUIRE(b == a); // the freed block is reused at the same address
    sa.release_resident(2, b);
}

TEST_CASE("StreamingAllocator staging forwards to the lock-free ring", "[memory][streaming]")
{
    StreamingAllocator sa(small_cfg());
    REQUIRE(sa.staging_capacity() == (crd::usize{1} << 20));

    void* s = sa.try_stage(4096, 64);
    REQUIRE(s != nullptr);
    std::memset(s, 0x5A, 4096);

    sa.begin_staging_epoch(1);
    void* s2 = sa.try_stage(4096, 64);
    REQUIRE(s2 != nullptr);
    sa.begin_staging_epoch(2);
    sa.retire_staging(1); // frees epochs 0 and 1
    void* s3 = sa.try_stage(4096, 64);
    REQUIRE(s3 != nullptr);
}
