// DIAG.3a -- the same boundary contract across every IAllocator: a size or alignment that cannot be satisfied returns
// nullptr from try_allocate (the non-fatal path) instead of wrapping into a small allocation, the allocator stays
// usable afterwards, earlier allocations keep their bytes, and a foreign pointer is never owned. The ownership cases
// at the end cover a block freed into the wrong allocator, child arenas torn down on their parent, arenas over
// external buffers, and construction that fails part way.
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
#include <crd/memory/construct.hpp>
#include <crd/memory/diagnostic_allocator.hpp>

#include <crd/core/assert.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/diag/specimen_runner.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <exception>
#include <utility>

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

// ---- DIAG.3a part 4: wrong-allocator free, parent/child destruction, external buffers, partial construction ----

namespace
{
// How a receiver answers a free of a pointer it never handed out.
enum class ForeignFree : crd::u8
{
    Ignored,   // an arena: deallocate is a no-op for every pointer, so a foreign one changes nothing
    Refused,   // refused in every build, after one assert where asserts are compiled in
    Reported,  // the diagnostic decorator: an UnknownPointer violation, no assert
    Undetected // MallocAllocator: owns() is true for any pointer, so it cannot tell (use the decorator)
};

struct Member
{
    mem::IAllocator* allocator;
    const char* name;
    ForeignFree foreign;
};

void ignore_violation(const mem::Violation& /*v*/, void* /*user*/) {}

constexpr crd::usize kFamilyMembers = 12U;

mem::StreamingAllocator::Config family_streaming_config()
{
    mem::StreamingAllocator::Config cfg;
    cfg.reserve_bytes = crd::usize{256} << 20;
    cfg.resident_chunk_bytes = crd::usize{1} << 20;
    cfg.staging_bytes = crd::usize{1} << 20;
    return cfg;
}

mem::VirtualMemoryAllocator::Config family_vm_config()
{
    mem::VirtualMemoryAllocator::Config cfg;
    cfg.reserve_bytes = crd::usize{64} << 20;
    return cfg;
}

// One instance of every IAllocator. Two families give every ordered pair of distinct allocators, including two
// allocators of the same type, which is the most likely wrong-allocator free in practice.
struct Family
{
    mem::MallocAllocator malloc{"fam-malloc"};
    mem::LinearAllocator linear{64U * 1024U, nullptr, "fam-linear"};
    mem::StackAllocator stack{64U * 1024U, nullptr, "fam-stack"};
    mem::PoolAllocator pool{kBlock, 64U, 64U, nullptr, "fam-pool"};
    mem::GrowableLinearAllocator glinear{64U * 1024U, nullptr, "fam-glinear"};
    mem::GrowablePoolAllocator gpool{kBlock, 64U, 16U, nullptr, "fam-gpool"};
    mem::TlsfAllocator tlsf{256U * 1024U, nullptr, "fam-tlsf"};
    mem::GrowableTlsfAllocator gtlsf{256U * 1024U, nullptr, "fam-gtlsf"};
    mem::VirtualMemoryAllocator vm{family_vm_config(), "fam-vm"};
    mem::TlsfAllocator ts_inner{256U * 1024U, nullptr, "fam-ts-inner"};
    mem::ThreadSafeAllocator ts{&ts_inner, "fam-ts"};
    mem::StreamingAllocator streaming{family_streaming_config()};
    mem::StreamingCategoryAllocator category{&streaming, 0U};
    mem::TlsfAllocator diag_backing{256U * 1024U, nullptr, "fam-diag-backing"};
    mem::DiagnosticAllocator diag{&diag_backing, mem::DiagnosticConfig{}};

    Member members[kFamilyMembers] = {
        {&malloc, "MallocAllocator", ForeignFree::Undetected},
        {&linear, "LinearAllocator", ForeignFree::Ignored},
        {&stack, "StackAllocator", ForeignFree::Ignored},
        {&pool, "PoolAllocator", ForeignFree::Refused},
        {&glinear, "GrowableLinearAllocator", ForeignFree::Ignored},
        {&gpool, "GrowablePoolAllocator", ForeignFree::Refused},
        {&tlsf, "TlsfAllocator", ForeignFree::Refused},
        {&gtlsf, "GrowableTlsfAllocator", ForeignFree::Refused},
        {&vm, "VirtualMemoryAllocator", ForeignFree::Refused},
        {&ts, "ThreadSafeAllocator", ForeignFree::Refused},
        {&category, "StreamingCategoryAllocator", ForeignFree::Refused},
        {&diag, "DiagnosticAllocator", ForeignFree::Reported},
    };

    Family() { diag.set_violation_handler(&ignore_violation, nullptr); }

    // The structural walkers of the heaps that have one.
    [[nodiscard]] bool structures_valid() const
    {
        return pool.validate_structure() && tlsf.validate_structure() && ts_inner.validate_structure() &&
               diag_backing.validate_structure();
    }
};
} // namespace

TEST_CASE("allocator ownership: a block freed into the wrong allocator is refused or ignored and never adopted",
          "[memory][contract][diag]")
{
    Family sources;
    Family receivers;
    for (const Member& dst : receivers.members)
    {
        INFO("receiver: " << dst.name);
        {
            AssertCapture capture;
            dst.allocator->deallocate(nullptr); // the null boundary: a no-op everywhere
            CHECK(g_asserts_seen == 0);
        }
        if (dst.foreign == ForeignFree::Undetected)
        {
            continue; // MallocAllocator would hand the foreign block to the C runtime; it cannot tell
        }
        for (const Member& src : sources.members)
        {
            INFO("source: " << src.name);
            void* const block = src.allocator->try_allocate(kBlock, 16U);
            REQUIRE(block != nullptr);
            fill(block, 0xC3U);
            void* const own = dst.allocator->try_allocate(kBlock, 16U);
            REQUIRE(own != nullptr);
            fill(own, 0x3CU);
            const crd::u64 violations_before = receivers.diag.violation_count();
            {
                AssertCapture capture;
                dst.allocator->deallocate(block);
                CHECK(g_asserts_seen == (dst.foreign == ForeignFree::Refused ? kAssertsPerRefusal : 0));
            }
            CHECK(receivers.diag.violation_count() ==
                  violations_before + (dst.foreign == ForeignFree::Reported ? 1U : 0U));
            CHECK(holds(block, 0xC3U)); // the receiver wrote nothing into the foreign block
            CHECK(holds(own, 0x3CU));
            CHECK(receivers.structures_valid());

            void* next[4] = {};
            for (void*& n : next)
            {
                n = dst.allocator->try_allocate(kBlock, 16U);
                REQUIRE(n != nullptr);
                CHECK(n != block); // the foreign block was never adopted into the receiver's free list
            }
            for (void* n : next)
            {
                dst.allocator->deallocate(n);
            }
            dst.allocator->deallocate(own);
            src.allocator->deallocate(block); // and its own allocator still frees it normally
        }
    }
    CHECK(sources.structures_valid());
    CHECK(receivers.structures_valid());
    CHECK(sources.diag.violation_count() == 0U);
}

TEST_CASE("allocator ownership: an interior pointer or an immediate double free is refused where the heap can tell",
          "[memory][contract][diag]")
{
    SECTION("tlsf")
    {
        mem::TlsfAllocator a(256U * 1024U, nullptr, "own-tlsf");
        void* const p = a.allocate(kBlock, 16U);
        void* const q = a.allocate(kBlock, 16U);
        fill(q, 0x71U);
        a.deallocate(p);
        {
            AssertCapture capture;
            a.deallocate(p);                               // an immediate double free
            a.deallocate(static_cast<crd::u8*>(q) + 8);    // misaligned, so never a payload
            a.deallocate(static_cast<crd::u8*>(q) - 4096); // below the first payload
            CHECK(g_asserts_seen == 3 * kAssertsPerRefusal);
        }
        CHECK(a.validate_structure());
        CHECK(holds(q, 0x71U));
        a.deallocate(q);
        CHECK(a.validate_structure());
    }
    SECTION("pool")
    {
        mem::PoolAllocator a(kBlock, 16U, 64U, nullptr, "own-pool");
        void* const p = a.allocate(kBlock, 16U);
        {
            AssertCapture capture;
            a.deallocate(static_cast<crd::u8*>(p) + 8); // inside a slot, not on its boundary
            CHECK(g_asserts_seen == kAssertsPerRefusal);
        }
        CHECK(a.validate_structure());
        CHECK(a.slots_in_use() == 1U);
        a.deallocate(p);
    }
    SECTION("growable pool")
    {
        mem::GrowablePoolAllocator a(kBlock, 64U, 4U, nullptr, "own-gpool");
        void* blocks[12] = {};
        for (void*& b : blocks)
        {
            b = a.allocate(kBlock, 16U); // three pages, so the ownership search has more than one to choose from
        }
        {
            AssertCapture capture;
            a.deallocate(static_cast<crd::u8*>(blocks[5]) + 8);
            CHECK(g_asserts_seen == kAssertsPerRefusal);
        }
        CHECK(a.slots_in_use() == 12U);
        for (void* b : blocks)
        {
            CHECK(a.owns(b));
            a.deallocate(b);
        }
        CHECK(a.slots_in_use() == 0U);
    }
}

namespace
{
// The parent every child below is built on: the diagnostic decorator over a TLSF heap. After a child is destroyed it
// must have returned every block it took (live_count() == 0), and freed nothing it did not take (no violation).
struct Parent
{
    static mem::DiagnosticConfig config()
    {
        mem::DiagnosticConfig cfg;
        cfg.capture_stacks = false;
        return cfg;
    }

    mem::TlsfAllocator heap{crd::usize{8} << 20, nullptr, "child-parent-heap"};
    mem::DiagnosticAllocator diag{&heap, config()};

    Parent() { diag.set_violation_handler(&ignore_violation, nullptr); }

    void check_clean() const
    {
        CHECK(diag.live_count() == 0U);
        CHECK(diag.violation_count() == 0U);
        CHECK(diag.check_all_redzones() == 0U); // no child wrote outside the blocks it was given
        CHECK(heap.validate_structure());
    }
};
} // namespace

TEST_CASE("allocator ownership: destroying a child arena returns exactly what it took from its parent",
          "[memory][contract][diag]")
{
    Parent parent;
    SECTION("linear, stack, pool, tlsf, ring and offset")
    {
        {
            mem::LinearAllocator linear(16U * 1024U, &parent.diag, "child-linear");
            mem::StackAllocator stack(16U * 1024U, &parent.diag, "child-stack");
            mem::PoolAllocator pool(kBlock, 32U, 16U, &parent.diag, "child-pool");
            mem::TlsfAllocator tlsf(64U * 1024U, &parent.diag, "child-tlsf");
            mem::RingAllocator ring(4096U, &parent.diag, 4U, "child-ring");
            mem::OffsetAllocator offset(crd::u32{1} << 20, 64U, &parent.diag, "child-offset");
            CHECK(parent.diag.live_count() == 7U); // one block each, two node arrays for the offset allocator
            fill(linear.allocate(kBlock, 16U), 0x01U);
            fill(stack.allocate(kBlock, 16U), 0x02U);
            fill(pool.allocate(kBlock, 16U), 0x03U);
            fill(tlsf.allocate(kBlock, 16U), 0x04U);
            fill(ring.try_claim(kBlock, 16U), 0x05U);
            CHECK(offset.allocate(4096U).valid());
        }
        parent.check_clean();
    }
    SECTION("growable arenas across several chunks and pages")
    {
        {
            mem::GrowableLinearAllocator glinear(4096U, &parent.diag, "child-glinear");
            for (int i = 0; i < 6; ++i)
            {
                fill(glinear.allocate(3000U, 16U), 0x11U); // a new chunk almost every time
            }
            void* const big = glinear.allocate(20000U, 16U); // an oversized request gets its own chunk
            REQUIRE(big != nullptr);

            mem::GrowablePoolAllocator gpool(kBlock, 64U, 4U, &parent.diag, "child-gpool");
            void* slots[24] = {};
            for (void*& s : slots)
            {
                s = gpool.allocate(kBlock, 16U); // six pages: the page table grows from 4 to 8 entries
                fill(s, 0x22U);
            }
            CHECK(gpool.page_count() == 6U);
            gpool.deallocate(slots[3]);

            mem::GrowableTlsfAllocator gtlsf(64U * 1024U, &parent.diag, "child-gtlsf");
            void* blocks[4] = {};
            for (void*& b : blocks)
            {
                b = gtlsf.allocate(40U * 1024U, 16U); // one chunk each
                REQUIRE(b != nullptr);
            }
            CHECK(gtlsf.num_chunks() >= 3U);
            gtlsf.deallocate(blocks[1]);
        }
        parent.check_clean();
    }
    SECTION("a nested chain torn down innermost first")
    {
        {
            mem::TlsfAllocator outer(256U * 1024U, &parent.diag, "chain-tlsf");
            mem::LinearAllocator middle(64U * 1024U, &outer, "chain-linear");
            mem::StackAllocator inner(4096U, &middle, "chain-stack");
            fill(inner.allocate(kBlock, 16U), 0x33U);
            CHECK(parent.diag.live_count() == 1U); // only the outer arena reaches the parent
        }
        parent.check_clean();
    }
    SECTION("a diagnostic decorator whose metadata comes from the parent")
    {
        {
            mem::TlsfAllocator backing(64U * 1024U, &parent.diag, "child-diag-backing");
            mem::DiagnosticAllocator child(&backing, Parent::config(), &parent.diag, "child-diag");
            fill(child.allocate(kBlock, 16U), 0x44U);
        }
        parent.check_clean();
    }
    SECTION("a moved growable pool frees its pages once")
    {
        {
            mem::GrowablePoolAllocator a(kBlock, 64U, 4U, &parent.diag, "move-gpool-a");
            for (int i = 0; i < 10; ++i)
            {
                fill(a.allocate(kBlock, 16U), 0x55U);
            }
            mem::GrowablePoolAllocator b(std::move(a)); // a keeps no pages
            mem::GrowablePoolAllocator c(kBlock, 64U, 4U, &parent.diag, "move-gpool-c");
            fill(c.allocate(kBlock, 16U), 0x66U);
            c = std::move(b); // c returns its own page and table, then takes b's
            CHECK(c.slots_in_use() == 10U);
        }
        parent.check_clean();
    }
}

TEST_CASE("allocator ownership: an arena over an external buffer stays inside it and never frees it",
          "[memory][contract][diag]")
{
    constexpr crd::usize arena_bytes = 16U * 1024U;
    constexpr crd::usize guard_bytes = 64U;
    constexpr crd::u8 guard_byte = 0xEEU;
    Parent parent;
    auto* const raw = static_cast<crd::u8*>(parent.diag.allocate(arena_bytes + 2U * guard_bytes, 64U));
    REQUIRE(raw != nullptr);
    std::memset(raw, guard_byte, arena_bytes + 2U * guard_bytes);
    crd::u8* const buffer = raw + guard_bytes;

    // Fill the arena to exhaustion through `a`; every block must lie inside the buffer.
    const auto use_up = [buffer](mem::IAllocator& a, crd::usize block)
    {
        crd::usize n = 0U;
        for (void* p = a.try_allocate(block, 16U); p != nullptr; p = a.try_allocate(block, 16U))
        {
            const auto* const b = static_cast<const crd::u8*>(p);
            REQUIRE(b >= buffer);
            REQUIRE(b + block <= buffer + arena_bytes);
            std::memset(p, 0x5AU, block);
            ++n;
        }
        CHECK(n > 0U);
    };

    SECTION("linear")
    {
        mem::LinearAllocator a(buffer, arena_bytes, "ext-linear");
        use_up(a, 256U);
    }
    SECTION("stack")
    {
        mem::StackAllocator a(buffer, arena_bytes, "ext-stack");
        use_up(a, 256U);
    }
    SECTION("pool")
    {
        mem::PoolAllocator a(buffer, kBlock, arena_bytes / kBlock, 16U, "ext-pool");
        use_up(a, kBlock);
    }
    SECTION("tlsf")
    {
        mem::TlsfAllocator a(buffer, arena_bytes, "ext-tlsf");
        use_up(a, 256U);
    }

    // The arena is gone. The buffer is still its owner's block, untouched outside the arena, and fully usable again
    // (under AddressSanitizer this also proves the arena unpoisoned what it had poisoned).
    for (crd::usize i = 0; i < guard_bytes; ++i)
    {
        CHECK(raw[i] == guard_byte);
        CHECK(raw[guard_bytes + arena_bytes + i] == guard_byte);
    }
    CHECK(parent.diag.live_count() == 1U);
    std::memset(buffer, 0, arena_bytes);
    parent.diag.deallocate(raw);
    parent.check_clean();
}

namespace
{
int g_live_widgets = 0;
int g_widget_countdown = -1; // the construction that throws; -1 never throws
int g_last_destroyed_id = -1;
int g_next_widget_id = 0;

struct ConstructionFailed : std::exception
{
};

// Counts live instances, and throws from its constructor when the countdown reaches zero.
struct Widget
{
    Widget() : id(g_next_widget_id++)
    {
        if (g_widget_countdown == 0)
        {
            g_widget_countdown = -1;
            throw ConstructionFailed{};
        }
        if (g_widget_countdown > 0)
        {
            --g_widget_countdown;
        }
        ++g_live_widgets;
    }
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;
    ~Widget()
    {
        CHECK(id == g_last_destroyed_id - 1); // destroyed in reverse order of construction
        g_last_destroyed_id = id;
        --g_live_widgets;
    }

    int id;
    int payload[4] = {};
};

void reset_widgets(int countdown, int count)
{
    g_live_widgets = 0;
    g_widget_countdown = countdown;
    g_next_widget_id = 0;
    g_last_destroyed_id = count; // so the last-built widget (id count - 1) is the first one expected
}
} // namespace

TEST_CASE("allocator ownership: partial construction leaks neither objects nor memory", "[memory][contract][diag]")
{
    Parent parent;
    SECTION("a refused allocation constructs nothing")
    {
        mem::LinearAllocator tiny(64U, &parent.diag, "partial-tiny");
        void* const used = tiny.allocate(64U, 16U);
        REQUIRE(used != nullptr);
        reset_widgets(-1, 0);
        CHECK(mem::construct<Widget>(tiny) == nullptr); // never a placement new on nullptr
        CHECK(mem::construct_array<Widget>(tiny, 4U) == nullptr);
        CHECK(g_next_widget_id == 0);
    }
    SECTION("an array whose byte size overflows is refused before the allocator is asked")
    {
        CHECK(mem::allocate_array<crd::u64>(parent.diag, SIZE_MAX / 4U) == nullptr);
        CHECK(mem::construct_array<Widget>(parent.diag, SIZE_MAX / 2U) == nullptr);
        CHECK(parent.diag.sampling_report().total_count == 0U);
    }
    SECTION("a constructor that throws midway through an array")
    {
        reset_widgets(5, 5); // widgets 0..4 are built, the sixth throws
        CHECK_THROWS_AS(mem::construct_array<Widget>(parent.diag, 8U), ConstructionFailed);
        CHECK(g_live_widgets == 0);      // the five that were built were destroyed
        CHECK(g_last_destroyed_id == 0); // in reverse order, down to the first
    }
    SECTION("a constructor that throws for a single object")
    {
        reset_widgets(0, 0);
        CHECK_THROWS_AS(mem::construct<Widget>(parent.diag), ConstructionFailed);
        CHECK(g_live_widgets == 0);
    }
    SECTION("a complete array is destroyed in reverse and returned")
    {
        reset_widgets(-1, 3);
        Widget* const w = mem::construct_array<Widget>(parent.diag, 3U);
        REQUIRE(w != nullptr);
        CHECK(g_live_widgets == 3);
        mem::destroy_array(parent.diag, w, 3U);
        CHECK(g_live_widgets == 0);
    }
    parent.check_clean(); // every path returned its storage
}

namespace
{
// Grants `allowance` requests from the parent and then refuses, as an exhausted arena does.
class RefusingParent final : public mem::IAllocator
{
public:
    RefusingParent(mem::IAllocator& parent, int allowance) : m_parent(parent), m_allowance(allowance)
    {
        m_name = "refusing-parent";
    }

    void* allocate(crd::usize size, crd::usize alignment) override { return try_allocate(size, alignment); }
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize alignment) override
    {
        if (m_allowance <= 0)
        {
            return nullptr;
        }
        --m_allowance;
        return m_parent.try_allocate(size, alignment);
    }
    void deallocate(void* p) noexcept override { m_parent.deallocate(p); }
    [[nodiscard]] bool owns(const void* p) const noexcept override { return m_parent.owns(p); }

    void allow(int allowance) { m_allowance = allowance; }

private:
    mem::IAllocator& m_parent;
    int m_allowance;
};
} // namespace

TEST_CASE("allocator ownership: a growable pool whose parent refuses a page stays usable", "[memory][contract][diag]")
{
    Parent parent;
    {
        RefusingParent refusing(parent.diag, 2); // the page table and one page
        mem::GrowablePoolAllocator a(kBlock, 64U, 4U, &refusing, "refused-gpool");
        void* slots[4] = {};
        for (void*& s : slots)
        {
            s = a.try_allocate(kBlock, 16U);
            REQUIRE(s != nullptr);
            fill(s, 0x77U);
        }
        CHECK(a.try_allocate(kBlock, 16U) == nullptr); // the second page is refused: nullptr, not a write to null
        CHECK(a.page_count() == 1U);
        CHECK(holds(slots[0], 0x77U));
        a.deallocate(slots[2]);
        CHECK(a.try_allocate(kBlock, 16U) == slots[2]); // a freed slot is reusable without a new page

        refusing.allow(1);
        void* const grown = a.try_allocate(kBlock, 16U); // the parent grants again: the pool grows
        CHECK(grown != nullptr);
        CHECK(a.page_count() == 2U);
    }
    parent.check_clean();
}

TEST_CASE("allocator ownership: an arena whose parent refuses its backing memory reaches its out-of-memory fatal",
          "[memory][contract][diag][harness]")
{
    namespace cd = crd::diag;
    namespace cont = crd::containers;
    const char* const modes[] = {"linear-parent-refuses", "stack-parent-refuses", "pool-parent-refuses",
                                 "tlsf-parent-refuses",   "ring-parent-refuses",  "offset-parent-refuses",
                                 "gpool-parent-refuses"};
    for (const char* mode : modes)
    {
        cd::Expectation e;
        e.want = cd::Expectation::Want::CleanExit; // exit_code is the oracle here, not the verdict
        cont::Array<cont::String> args;
        args.push_back(cont::String{mode});
        const cd::Outcome o = cd::run_specimen(cont::String{CRD_DIAG_ALLOC_FATAL_SPECIMEN}, args, e);
        INFO("mode=" << mode << " verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code
                     << " reason=" << o.reason.c_str());
        // 42 = the arena's own fatal fired; 97 = another assert fired first; 0 = the arena was built on null and
        // returned; 96 = unknown mode. A crash on the null buffer is none of these.
        CHECK(o.exit_code == 42);
    }
}
