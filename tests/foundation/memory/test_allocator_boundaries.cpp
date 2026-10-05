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
