// DIAG.3a -- the same boundary contract across every IAllocator: a size or alignment that cannot be satisfied returns
// nullptr from try_allocate (the non-fatal path) instead of wrapping into a small allocation, the allocator stays
// usable afterwards, earlier allocations keep their bytes, and a foreign pointer is never owned.
// Contract: docs/design/runtime-diagnostics.md#diag-3a.

#include <crd/memory/allocators/growable_linear_allocator.hpp>
#include <crd/memory/allocators/growable_pool_allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/allocators/offset_allocator.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/allocators/ring_allocator.hpp>
#include <crd/memory/allocators/stack_allocator.hpp>
#include <crd/memory/allocators/streaming_allocator.hpp>
#include <crd/memory/allocators/streaming_category_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/allocators/virtual_memory_allocator.hpp>
#include <crd/memory/diagnostic_allocator.hpp>

#include <crd/core/assert.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/diag/specimen_runner.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

namespace
{
namespace mem = crd::memory;

constexpr crd::usize kBlock = 64U;

// The sizes and alignments no allocator can satisfy: each must fail without wrapping.
constexpr crd::usize kHugeSizes[] = {SIZE_MAX, SIZE_MAX - 8U, SIZE_MAX - 4095U, SIZE_MAX / 2U + 1U}; // NOLINT
constexpr crd::usize kHugeAlignment = crd::usize{1} << (sizeof(crd::usize) * 8U - 2U);

[[nodiscard]] bool aligned(const void* p, crd::usize alignment)
{
    return (reinterpret_cast<std::uintptr_t>(p) & (alignment - 1U)) == 0U; // NOLINT(performance-no-int-to-ptr)
}

void fill(void* p, crd::u8 value)
{
    std::memset(p, value, kBlock);
}

[[nodiscard]] bool holds(const void* p, crd::u8 value)
{
    const auto* bytes = static_cast<const crd::u8*>(p);
    for (crd::usize i = 0; i < kBlock; ++i)
    {
        if (bytes[i] != value)
        {
            return false;
        }
    }
    return true;
}

// What owns() can answer. Two allocators document a fixed answer instead of tracking their blocks: MallocAllocator says
// true for every non-null pointer, and StreamingCategoryAllocator says false (ownership is the StreamingAllocator's).
enum class Ownership : crd::u8
{
    Exact,
    AlwaysTrue,
    AlwaysFalse,
};

// The shared contract.
void check_boundaries(mem::IAllocator& a, const char* name, Ownership ownership = Ownership::Exact)
{
    INFO("allocator: " << name);
    CHECK(a.try_allocate(0U, 16U) == nullptr); // zero size: nullptr, never a block and never an assert

    void* first = a.try_allocate(kBlock, 16U);
    REQUIRE(first != nullptr);
    CHECK(aligned(first, 16U));
    fill(first, 0xA5U);

    for (const crd::usize size : kHugeSizes)
    {
        INFO("size " << size);
        CHECK(a.try_allocate(size, 16U) == nullptr); // never a wrapped, small allocation
    }
    CHECK(a.try_allocate(kBlock, kHugeAlignment) == nullptr); // an unsatisfiable alignment never wraps the address

    void* second = a.try_allocate(kBlock, 32U); // still usable after every refusal
    REQUIRE(second != nullptr);
    CHECK(aligned(second, 32U));
    CHECK(second != first);
    fill(second, 0x5AU);
    CHECK(holds(first, 0xA5U)); // the earlier allocation kept its bytes

    int foreign = 0;
    if (ownership != Ownership::AlwaysTrue)
    {
        CHECK_FALSE(a.owns(&foreign)); // a foreign pointer is never owned
    }
    if (ownership != Ownership::AlwaysFalse)
    {
        CHECK(a.owns(first));
    }

    a.deallocate(second);
    a.deallocate(first);
}
} // namespace

TEST_CASE("allocator boundaries: every IAllocator refuses unsatisfiable requests without wrapping",
          "[memory][contract][diag]")
{
    SECTION("malloc")
    {
        mem::MallocAllocator a("bound-malloc");
        check_boundaries(a, "MallocAllocator", Ownership::AlwaysTrue);
    }
    SECTION("linear")
    {
        mem::LinearAllocator a(64U * 1024U, nullptr, "bound-linear");
        check_boundaries(a, "LinearAllocator");
    }
    SECTION("stack")
    {
        mem::StackAllocator a(64U * 1024U, nullptr, "bound-stack");
        check_boundaries(a, "StackAllocator");
    }
    SECTION("pool")
    {
        mem::PoolAllocator a(kBlock, 16U, 64U, nullptr, "bound-pool");
        check_boundaries(a, "PoolAllocator");
    }
    SECTION("growable linear")
    {
        mem::GrowableLinearAllocator a(64U * 1024U, nullptr, "bound-glinear");
        check_boundaries(a, "GrowableLinearAllocator");
    }
    SECTION("growable pool")
    {
        mem::GrowablePoolAllocator a(kBlock, 64U, 16U, nullptr, "bound-gpool");
        check_boundaries(a, "GrowablePoolAllocator");
    }
    SECTION("tlsf")
    {
        mem::TlsfAllocator a(256U * 1024U, nullptr, "bound-tlsf");
        check_boundaries(a, "TlsfAllocator");
    }
    SECTION("growable tlsf")
    {
        mem::GrowableTlsfAllocator a(256U * 1024U, nullptr, "bound-gtlsf");
        check_boundaries(a, "GrowableTlsfAllocator");
    }
    SECTION("virtual memory")
    {
        mem::VirtualMemoryAllocator::Config cfg;
        cfg.reserve_bytes = crd::usize{64} << 20;
        mem::VirtualMemoryAllocator a(cfg, "bound-vm");
        check_boundaries(a, "VirtualMemoryAllocator");
    }
    SECTION("thread-safe wrapper")
    {
        mem::TlsfAllocator           inner(256U * 1024U, nullptr, "bound-ts-inner");
        mem::ThreadSafeAllocator     a(&inner, "bound-ts");
        check_boundaries(a, "ThreadSafeAllocator");
    }
    SECTION("streaming category")
    {
        mem::StreamingAllocator::Config cfg;
        cfg.reserve_bytes        = crd::usize{256} << 20;
        cfg.resident_chunk_bytes = crd::usize{1} << 20;
        cfg.staging_bytes        = crd::usize{1} << 20;
        mem::StreamingAllocator         streaming(cfg);
        mem::StreamingCategoryAllocator a(&streaming, 0U);
        check_boundaries(a, "StreamingCategoryAllocator", Ownership::AlwaysFalse);
    }
    SECTION("diagnostic decorator")
    {
        mem::TlsfAllocator          backing(256U * 1024U, nullptr, "bound-diag-backing");
        mem::DiagnosticConfig       cfg;
        mem::DiagnosticAllocator    a(&backing, cfg);
        check_boundaries(a, "DiagnosticAllocator");
    }
}

namespace
{
constexpr crd::usize kMaxBlocks = 512U;

// Allocates `block`-byte blocks with try_allocate until it refuses. The refusal must come within kMaxBlocks, the
// allocator must survive it, and the first block must keep its bytes. Returns how many blocks succeeded; `blocks`
// receives them.
crd::usize exhaust(mem::IAllocator& a, crd::usize block, void* (&blocks)[kMaxBlocks])
{
    crd::usize n = 0U;
    while (n < kMaxBlocks)
    {
        void* p = a.try_allocate(block, 16U);
        if (p == nullptr)
        {
            break;
        }
        blocks[n] = p;
        ++n;
    }
    REQUIRE(n > 0U);
    REQUIRE(n < kMaxBlocks); // exhaustion was reached and refused, not an endless supply
    return n;
}
} // namespace

TEST_CASE("allocator boundaries: exhaustion refuses with nullptr and keeps every earlier allocation",
          "[memory][contract][diag]")
{
    void* blocks[kMaxBlocks] = {};
    SECTION("linear")
    {
        mem::LinearAllocator a(64U * 1024U, nullptr, "exhaust-linear");
        const crd::usize     n = exhaust(a, 4096U, blocks);
        fill(blocks[0], 0x11U);
        CHECK(a.try_allocate(4096U, 16U) == nullptr); // stays exhausted
        CHECK(holds(blocks[0], 0x11U));
        CHECK(a.owns(blocks[n - 1U]));
    }
    SECTION("stack")
    {
        mem::StackAllocator a(64U * 1024U, nullptr, "exhaust-stack");
        const crd::usize    n = exhaust(a, 4096U, blocks);
        fill(blocks[0], 0x22U);
        CHECK(a.try_allocate(4096U, 16U) == nullptr);
        CHECK(holds(blocks[0], 0x22U));
        CHECK(a.owns(blocks[n - 1U]));
    }
    SECTION("pool: a freed slot is reusable after exhaustion")
    {
        mem::PoolAllocator a(kBlock, 16U, 64U, nullptr, "exhaust-pool");
        const crd::usize   n = exhaust(a, kBlock, blocks);
        CHECK(n == 16U);
        fill(blocks[0], 0x33U);
        a.deallocate(blocks[n - 1U]);
        void* again = a.try_allocate(kBlock, 16U);
        CHECK(again != nullptr);
        CHECK(holds(blocks[0], 0x33U));
    }
    SECTION("tlsf: a freed block is reusable after exhaustion")
    {
        mem::TlsfAllocator a(256U * 1024U, nullptr, "exhaust-tlsf");
        const crd::usize   n = exhaust(a, 4096U, blocks);
        fill(blocks[0], 0x44U);
        a.deallocate(blocks[n - 1U]);
        void* again = a.try_allocate(4096U, 16U);
        CHECK(again != nullptr);
        CHECK(holds(blocks[0], 0x44U));
    }
    SECTION("virtual memory")
    {
        mem::VirtualMemoryAllocator::Config cfg;
        cfg.reserve_bytes = crd::usize{64} << 20;
        mem::VirtualMemoryAllocator a(cfg, "exhaust-vm");
        const crd::usize            n = exhaust(a, crd::usize{1} << 20, blocks);
        fill(blocks[0], 0x55U);
        CHECK(a.try_allocate(crd::usize{1} << 20, 16U) == nullptr);
        CHECK(holds(blocks[0], 0x55U));
        CHECK(a.owns(blocks[n - 1U]));
    }
}

TEST_CASE("allocator boundaries: a failed reallocate keeps the previous allocation", "[memory][contract][diag]")
{
    // The allocators whose allocate returns nullptr on failure. Like C realloc, reallocate must then return nullptr
    // and leave the old block allocated and unchanged. (The fatal-on-exhaustion allocators are covered by the
    // fatal-path specimen below; PoolAllocator's reallocate past its slot size is a precondition violation.)
    const auto check_realloc_failure = [](mem::IAllocator& a, const char* name)
    {
        INFO("allocator: " << name);
        void* p = a.allocate(kBlock, 16U);
        REQUIRE(p != nullptr);
        fill(p, 0x6CU);
        for (const crd::usize size : kHugeSizes)
        {
            INFO("size " << size);
            CHECK(a.reallocate(p, kBlock, size, 16U) == nullptr);
            CHECK(holds(p, 0x6CU)); // the old block is untouched
        }
        void* q = a.reallocate(p, kBlock, 2U * kBlock, 16U); // and a satisfiable reallocate still moves the bytes
        REQUIRE(q != nullptr);
        CHECK(holds(q, 0x6CU));
        a.deallocate(q);
    };
    SECTION("linear")
    {
        mem::LinearAllocator a(64U * 1024U, nullptr, "realloc-linear");
        check_realloc_failure(a, "LinearAllocator");
    }
    SECTION("stack")
    {
        mem::StackAllocator a(64U * 1024U, nullptr, "realloc-stack");
        check_realloc_failure(a, "StackAllocator");
    }
    SECTION("growable linear")
    {
        mem::GrowableLinearAllocator a(64U * 1024U, nullptr, "realloc-glinear");
        check_realloc_failure(a, "GrowableLinearAllocator");
    }
}

TEST_CASE("allocator boundaries: an unsatisfiable reallocate reaches the out-of-memory fatal and never a wrapped size",
          "[memory][contract][diag][harness]")
{
    namespace cd   = crd::diag;
    namespace cont = crd::containers;
    const char* const modes[] = {"tlsf-realloc-overflow", "tlsf-realloc-huge", "gtlsf-realloc-huge"};
    for (const char* mode : modes)
    {
        cd::Expectation e;
        e.want = cd::Expectation::Want::CleanExit; // exit_code is the oracle here, not the verdict
        cont::Array<cont::String> args;
        args.push_back(cont::String{mode});
        const cd::Outcome o = cd::run_specimen(cont::String{CRD_DIAG_ALLOC_FATAL_SPECIMEN}, args, e);
        INFO("mode=" << mode << " verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code
                     << " reason=" << o.reason.c_str());
        CHECK(cont::StringView{o.identity} == cont::StringView{"crd-diag-alloc-fatal-specimen"});
        // 42 = the expected fatal fired; 97 = a different assert fired first; 0 = reallocate returned (the wrap this
        // test exists to catch); 96 = unknown mode.
        CHECK(o.exit_code == 42);
    }
}

namespace
{
int g_asserts_seen = 0;

void count_assert(const char* /*expr*/, const char* /*file*/, int /*line*/, const char* /*msg*/) noexcept
{
    ++g_asserts_seen;
}

int continue_after_assert(const char* /*formatted*/) noexcept
{
    return 0; // continue: the code under test must then refuse on its own
}

// Counts asserts instead of breaking, so a refusal path can be run in-process; restores the previous handlers.
class AssertCapture
{
public:
    AssertCapture() : m_handler(crd::get_assert_handler()), m_platform(crd::get_assert_platform_handler())
    {
        g_asserts_seen = 0;
        crd::set_assert_handler(&count_assert);
        crd::set_assert_platform_handler(&continue_after_assert);
    }
    ~AssertCapture()
    {
        crd::set_assert_handler(m_handler);
        crd::set_assert_platform_handler(m_platform);
    }
    AssertCapture(const AssertCapture&)            = delete;
    AssertCapture& operator=(const AssertCapture&) = delete;

private:
    crd::AssertHandler         m_handler;
    crd::AssertPlatformHandler m_platform;
};

// One per refused call where asserts are compiled in; none where they are not. Either way the refusal must hold.
constexpr int kAssertsPerRefusal = CRD_ENABLE_ASSERTS ? 1 : 0;
} // namespace

TEST_CASE("allocator boundaries: OffsetAllocator refuses impossible requests and handles it did not issue",
          "[memory][contract][diag]")
{
    constexpr crd::u32 capacity = crd::u32{1} << 20;
    using Offset                = mem::OffsetAllocator;

    SECTION("sizes and alignments that cannot be honoured return an invalid allocation")
    {
        Offset a(capacity, 64U, nullptr, "bound-offset");
        CHECK_FALSE(a.allocate(0U).valid());
        CHECK_FALSE(a.allocate(UINT32_MAX).valid());
        CHECK_FALSE(a.allocate(capacity + 1U).valid());
        CHECK_FALSE(a.allocate(64U, crd::u32{1} << 31).valid()); // the padding alone exceeds the span
        CHECK_FALSE(a.allocate(64U, 3U).valid());                 // not a power of two
        CHECK(a.free_storage() == capacity);                     // nothing was taken by a refusal
        const Offset::Allocation whole = a.allocate(capacity);   // the boundary itself still fits
        CHECK(whole.valid());
        a.free(whole);
        CHECK(a.free_storage() == capacity);
    }
    SECTION("an exhausted node pool refuses instead of overrunning the side arrays")
    {
        // Four nodes: three live allocations each split the free region, using the fourth node for the last
        // remainder. A fourth split has no node left.
        Offset                   a(capacity, 4U, nullptr, "bound-offset-nodes");
        const Offset::Allocation x = a.allocate(64U);
        const Offset::Allocation y = a.allocate(64U);
        const Offset::Allocation z = a.allocate(64U);
        REQUIRE(x.valid());
        REQUIRE(y.valid());
        REQUIRE(z.valid());
        const crd::u32 free_before = a.free_storage();
        CHECK_FALSE(a.allocate(64U).valid());
        CHECK(a.free_storage() == free_before); // the refusal changed nothing

        a.free(x); // a freed node makes room again; an exact-fit request needs no split at all
        const Offset::Allocation again = a.allocate(64U);
        CHECK(again.valid());
        a.free(again);
        a.free(y);
        a.free(z);
        CHECK(a.free_storage() == capacity); // every region coalesced back: the bins are intact
    }
    SECTION("a foreign, stale or double-freed handle is refused without touching the bins")
    {
        Offset                   a(capacity, 64U, nullptr, "bound-offset-free");
        const Offset::Allocation x = a.allocate(64U);
        const Offset::Allocation y = a.allocate(64U);
        REQUIRE(x.valid());
        REQUIRE(y.valid());
        a.free(x);
        const crd::u32 free_before = a.free_storage();
        {
            AssertCapture capture;
            a.free(Offset::Allocation{0U, 9999U});            // a node index this allocator never had
            a.free(x);                                         // a double free
            a.free(Offset::Allocation{y.offset + 4096U, y.metadata}); // a live node, but not this offset
            CHECK(g_asserts_seen == 3 * kAssertsPerRefusal);
        }
        CHECK(a.free_storage() == free_before);
        a.free(y);
        CHECK(a.free_storage() == capacity);
    }
}

TEST_CASE("allocator boundaries: RingAllocator refuses impossible claims and recovers after retire",
          "[memory][contract][diag]")
{
    constexpr crd::usize capacity = 4096U;
    mem::RingAllocator   ring(capacity, nullptr, 4U, "bound-ring");
    CHECK(ring.try_claim(0U) == nullptr);
    CHECK(ring.try_claim(capacity + 1U) == nullptr);
    CHECK(ring.try_claim(SIZE_MAX) == nullptr);
    CHECK(ring.try_claim(64U, 2U * mem::kCachelineSize) == nullptr); // above the buffer's own alignment
    CHECK(ring.try_claim(64U, 3U) == nullptr);                       // not a power of two
    CHECK(ring.in_use_bytes() == 0U);                                // no refusal consumed space

    void* claims[4] = {};
    for (void*& c : claims)
    {
        c = ring.try_claim(1024U, 16U);
        REQUIRE(c != nullptr);
        fill(c, 0x7EU);
    }
    CHECK(ring.try_claim(1024U, 16U) == nullptr); // full until the epoch retires
    CHECK(holds(claims[0], 0x7EU));

    ring.begin_epoch(1U); // closes epoch 0, which holds every claim above
    ring.retire(0U);
    CHECK(ring.in_use_bytes() == 0U);
    CHECK(ring.try_claim(1024U, 16U) != nullptr); // the space is reusable
}
