// DIAG.3a -- the same boundary contract across every IAllocator: a size or alignment that cannot be satisfied returns
// nullptr from try_allocate (the non-fatal path) instead of wrapping into a small allocation, the allocator stays
// usable afterwards, earlier allocations keep their bytes, and a foreign pointer is never owned.
// Contract: docs/design/runtime-diagnostics.md#diag-3a.

#include <crd/memory/allocators/growable_linear_allocator.hpp>
#include <crd/memory/allocators/growable_pool_allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/allocators/stack_allocator.hpp>
#include <crd/memory/allocators/streaming_allocator.hpp>
#include <crd/memory/allocators/streaming_category_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/allocators/virtual_memory_allocator.hpp>
#include <crd/memory/diagnostic_allocator.hpp>

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
