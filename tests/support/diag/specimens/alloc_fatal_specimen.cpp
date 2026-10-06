// DIAG.3a fatal-path control: a reallocate that no pool can satisfy must end in the allocator's out-of-memory fatal,
// never in a wrapped size that hands back a block too small for the request. The fatal path cannot run in-process
// (CRD_FATAL breaks into the debugger or aborts), so the memory tests run this specimen as a bounded child, once per
// mode (argv[1]):
//   tlsf-realloc-overflow  TlsfAllocator::reallocate(p, 64, SIZE_MAX): the size round-up overflows
//   tlsf-realloc-huge      TlsfAllocator::reallocate(p, 64, SIZE_MAX / 2 + 1): no overflow, the pool is too small
//   gtlsf-realloc-huge     GrowableTlsfAllocator::reallocate(p, 64, SIZE_MAX): larger than any chunk
//
// The assert platform handler turns the expected fatal into exit 42 and any other assert into 97 (as in the
// observer-swap specimen). If reallocate returns at all, the specimen exits 0, which the consuming test rejects, so a
// missing check cannot pass. An unknown mode exits 96.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "specimen_common.hpp"

#include <crd/core/assert.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
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
    return false;
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
