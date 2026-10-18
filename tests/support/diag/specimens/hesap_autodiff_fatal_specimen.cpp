// hesap-autodiff contract fatals (crd-hesap-autodiff-fatal-* CTests, tests/numerics/hesap-autodiff). The vectorized
// HVP tape takes its node and scratch arrays from the caller's allocator and records into them up to a fixed node
// capacity. A refused array (a sub-budget allocator returns nullptr), a scratch pool size that overflows and a node
// past the capacity are each fatal in every build, never a null, short or overrun array written (DIAG.3a; the arrays
// were written through null, and make() wrote past the capacity behind an assert a build without asserts drops,
// before). One mode per run (argv[1]); see expected_fatal.hpp for the exit protocol:
//   vtape-nodes-refused     VTape(n = 8, 4 nodes): the node array is refused
//   vtape-pool-refused      VTape(n = 8, 4 nodes): the node array is granted, the scratch pool refused
//   vtape-pool-overflow     VTape(n = INT_MAX, INT_MAX nodes): the pool size overflows, before any allocation
//   vtape-capacity-exceeded VTape(n = 8, 2 nodes): a third make()
// The refusing allocator is refusing_allocator.hpp.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/hesap/autodiff/vhvp.hpp>

#include <climits>
#include <cstring>

namespace
{
namespace vec = crd::hesap::autodiff::reverse::vec;
using crd_diag_fatal::RefusingAllocator;

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    RefusingAllocator alloc("hesap-autodiff-fatal-refusing");
    if (is(mode, "vtape-nodes-refused"))
    {
        crd_diag_fatal::arm(mode, vec::detail::kVTapeOutOfMemory);
        alloc.refuse_all();
        vec::VTape tape(&alloc, 8, 4);
        return tape.count() == 0U;
    }
    if (is(mode, "vtape-pool-refused"))
    {
        crd_diag_fatal::arm(mode, vec::detail::kVTapeOutOfMemory);
        alloc.refuse_after(1U); // the node array, then nothing
        vec::VTape tape(&alloc, 8, 4);
        return tape.count() == 0U;
    }
    if (is(mode, "vtape-pool-overflow"))
    {
        crd_diag_fatal::arm(mode, vec::detail::kVTapeSizeOverflow);
        vec::VTape tape(&alloc, INT_MAX, INT_MAX);
        return tape.count() == 0U;
    }
    if (is(mode, "vtape-capacity-exceeded"))
    {
        crd_diag_fatal::arm(mode, vec::detail::kVTapeCapacityExceeded);
        vec::VTape tape(&alloc, 8, 2);
        (void)tape.make(vec::VOp::Leaf, 8);
        (void)tape.make(vec::VOp::Leaf, 8);
        (void)tape.make(vec::VOp::Leaf, 8); // past the two nodes the arrays hold
        return true;
    }
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when the violation returned: the fatal did not fire, and the CTest (the marker line) rejects it.
    return run_mode(argv[1]) ? 0 : 96;
}
