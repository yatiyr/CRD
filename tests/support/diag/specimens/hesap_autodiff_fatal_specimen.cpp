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
// batch_gradient keeps each job's leaves on its stack and writes grad, gbuf and one tape per job; more than 32
// parameters, or a span shorter than the batch needs, is fatal in every build, never a stack or span overrun:
//   batch-too-many-params   33 parameters
//   batch-short-gbuf        S = 4, n = 2 and a gbuf of 7
//   batch-short-tapes       S = 4 and no tape
// The refusing allocator is refusing_allocator.hpp.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/hesap/autodiff/reverse.hpp>
#include <crd/hesap/autodiff/vhvp.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <climits>
#include <cstring>

namespace
{
namespace rev = crd::hesap::autodiff::reverse;
namespace vec = crd::hesap::autodiff::reverse::vec;
using crd_diag_fatal::RefusingAllocator;

struct SumLoss
{
    [[nodiscard]] rev::Var operator()(const rev::Var* theta, int n, int) const
    {
        rev::Var acc = theta[0];
        for (int i = 1; i < n; ++i)
        {
            acc = acc + theta[i];
        }
        return acc;
    }
};

// batch_gradient with S = 4 on the given spans; returns true when it came back.
[[nodiscard]] bool run_batch(int n, crd::usize gbuf_size, crd::usize tape_count)
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    rev::Tape tape(&alloc);
    rev::Tape* tapes[1] = {&tape};
    crd::f64 theta[33] = {};
    crd::f64 grad[33] = {};
    crd::f64 gbuf[4 * 33] = {};
    rev::batch_gradient(SumLoss{}, {theta, static_cast<crd::usize>(n)}, 4, {grad, static_cast<crd::usize>(n)},
                        {tapes, tape_count}, {gbuf, gbuf_size}, 1U);
    return true;
}

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
    if (is(mode, "batch-too-many-params"))
    {
        crd_diag_fatal::arm(mode, rev::detail::kBatchTooManyParams);
        return run_batch(33, 4 * 33, 1);
    }
    if (is(mode, "batch-short-gbuf"))
    {
        crd_diag_fatal::arm(mode, rev::detail::kBatchShortSpan);
        return run_batch(2, 7, 1);
    }
    if (is(mode, "batch-short-tapes"))
    {
        crd_diag_fatal::arm(mode, rev::detail::kBatchShortSpan);
        return run_batch(2, 8, 0);
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
