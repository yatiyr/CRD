// DIAG.3a fatal-path control: a reallocate that no pool can satisfy must end in the allocator's out-of-memory fatal,
// never in a wrapped size that hands back a block too small for the request. The fatal path cannot run in-process
// (CRD_FATAL breaks into the debugger or aborts), so the memory tests run this specimen as a bounded child, once per
// mode (argv[1]):
//   tlsf-realloc-overflow  TlsfAllocator::reallocate(p, 64, SIZE_MAX): the size round-up overflows
//   tlsf-realloc-huge      TlsfAllocator::reallocate(p, 64, SIZE_MAX / 2 + 1): no overflow, the pool is too small
//   gtlsf-realloc-huge     GrowableTlsfAllocator::reallocate(p, 64, SIZE_MAX): larger than any chunk
//
// Partial construction: an arena whose parent refuses its backing memory (an exhausted arena returns nullptr from
// allocate) must reach its own fatal instead of building itself on null:
//   linear-parent-refuses, stack-parent-refuses, pool-parent-refuses, tlsf-parent-refuses, ring-parent-refuses
//                          the owning constructor's single backing buffer is refused
//   offset-parent-refuses  the node array is granted and the free-node array is refused
//   gpool-parent-refuses   GrowablePoolAllocator::allocate when the parent refuses the first page
//
// The assert platform handler turns the expected fatal into exit 42 and any other assert into 97 (as in the
// observer-swap specimen). If reallocate returns at all, the specimen exits 0, which the consuming test rejects, so a
// missing check cannot pass. An unknown mode exits 96.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "specimen_common.hpp"

#include <crd/core/assert.hpp>
#include <crd/memory/allocators/growable_pool_allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/offset_allocator.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/allocators/ring_allocator.hpp>
#include <crd/memory/allocators/stack_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
const char* g_expected_fatal = nullptr; // the CRD_FATAL message this mode must reach

int alloc_fatal_assert_handler(const char* formatted_message)
{
    if (g_expected_fatal != nullptr && formatted_message != nullptr &&
        std::strstr(formatted_message, g_expected_fatal) != nullptr)
    {
        std::_Exit(42); // the expected fatal fired
    }
    std::_Exit(97); // some other assert fired first
}

// A parent that grants `allowance` requests from the default allocator and then refuses every later one, returning
// nullptr from allocate as an exhausted arena does.
class RefusingParent final : public crd::memory::IAllocator
{
public:
    explicit RefusingParent(int allowance) noexcept : m_allowance(allowance) { m_name = "alloc-fatal-refusing"; }

    void* allocate(crd::usize size, crd::usize alignment) override { return try_allocate(size, alignment); }
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize alignment) override
    {
        if (m_allowance <= 0)
        {
            return nullptr;
        }
        --m_allowance;
        return crd::memory::default_allocator()->try_allocate(size, alignment);
    }
    void deallocate(void* p) noexcept override { crd::memory::default_allocator()->deallocate(p); }
    [[nodiscard]] bool owns(const void* p) const noexcept override { return p != nullptr; }

private:
    int m_allowance;
};

// The parent-refuses modes. Returns false for an unknown mode.
[[nodiscard]] bool run_parent_refuses_mode(const char* mode)
{
    namespace mem = crd::memory;
    constexpr crd::usize bytes = crd::usize{64} * 1024U;
    RefusingParent none(0);
    if (std::strcmp(mode, "linear-parent-refuses") == 0)
    {
        g_expected_fatal = "LinearAllocator: parent refused the backing buffer";
        mem::LinearAllocator a(bytes, &none, "alloc-fatal-linear");
        (void)a.allocate(64U, 16U);
        return true;
    }
    if (std::strcmp(mode, "stack-parent-refuses") == 0)
    {
        g_expected_fatal = "StackAllocator: parent refused the backing buffer";
        mem::StackAllocator a(bytes, &none, "alloc-fatal-stack");
        (void)a.allocate(64U, 16U);
        return true;
    }
    if (std::strcmp(mode, "pool-parent-refuses") == 0)
    {
        g_expected_fatal = "PoolAllocator: parent refused the backing buffer";
        mem::PoolAllocator a(64U, 16U, 16U, &none, "alloc-fatal-pool");
        (void)a.allocate(64U, 16U);
        return true;
    }
    if (std::strcmp(mode, "tlsf-parent-refuses") == 0)
    {
        g_expected_fatal = "TlsfAllocator: parent refused the backing buffer";
        mem::TlsfAllocator a(bytes, &none, "alloc-fatal-tlsf");
        (void)a.try_allocate(64U, 16U);
        return true;
    }
    if (std::strcmp(mode, "ring-parent-refuses") == 0)
    {
        g_expected_fatal = "RingAllocator: parent refused the backing buffer";
        mem::RingAllocator a(bytes, &none, 4U, "alloc-fatal-ring");
        (void)a.try_claim(64U, 16U);
        return true;
    }
    if (std::strcmp(mode, "offset-parent-refuses") == 0)
    {
        g_expected_fatal = "OffsetAllocator: parent refused the node arrays";
        RefusingParent one(1); // the node array is granted, the free-node array is not
        mem::OffsetAllocator a(crd::u32{1} << 20, 64U, &one, "alloc-fatal-offset");
        (void)a.allocate(64U);
        return true;
    }
    if (std::strcmp(mode, "gpool-parent-refuses") == 0)
    {
        g_expected_fatal = "GrowablePoolAllocator: out of memory";
        mem::GrowablePoolAllocator a(64U, 16U, 16U, &none, "alloc-fatal-gpool");
        (void)a.allocate(64U, 16U);
        return true;
    }
    return false;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    constexpr crd::usize pool_bytes = crd::usize{256} * 1024U;
    if (std::strcmp(mode, "tlsf-realloc-overflow") == 0)
    {
        g_expected_fatal = "TlsfAllocator: reallocate size overflows";
        crd::memory::TlsfAllocator a(pool_bytes, nullptr, "alloc-fatal-tlsf");
        void* const p = a.allocate(64U, 16U);
        (void)a.reallocate(p, 64U, SIZE_MAX, 16U);
        return true;
    }
    if (std::strcmp(mode, "tlsf-realloc-huge") == 0)
    {
        g_expected_fatal = "TlsfAllocator: out of memory";
        crd::memory::TlsfAllocator a(pool_bytes, nullptr, "alloc-fatal-tlsf");
        void* const p = a.allocate(64U, 16U);
        (void)a.reallocate(p, 64U, SIZE_MAX / 2U + 1U, 16U);
        return true;
    }
    if (std::strcmp(mode, "gtlsf-realloc-huge") == 0)
    {
        g_expected_fatal = "GrowableTlsfAllocator: out of memory";
        crd::memory::GrowableTlsfAllocator a(pool_bytes, nullptr, "alloc-fatal-gtlsf");
        void* const p = a.allocate(64U, 16U);
        (void)a.reallocate(p, 64U, SIZE_MAX, 16U);
        return true;
    }
    return run_parent_refuses_mode(mode);
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd::set_assert_platform_handler(&alloc_fatal_assert_handler);
    crd_diag_announce();
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when reallocate returned: the fatal did not fire, and the test's oracle (42) rejects it.
    return run_mode(argv[1]) ? 0 : 96;
}
