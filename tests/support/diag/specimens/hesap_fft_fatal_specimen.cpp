// hesap-fft contract fatals (crd-hesap-fft-fatal-* CTests, tests/numerics/hesap-fft). A plan allocates its raw tables
// and its sub-plans from the caller's allocator; one that refuses (a sub-budget allocator returns nullptr) leaves the
// plan out of memory, fatal in every build, never a null table written (DIAG.3a; the tables and sub-plans were
// written through the refused pointer before). One mode per run (argv[1]); see expected_fatal.hpp for the exit
// protocol:
//   plan-hier-table-refused       FftPlan<f64>(4096): the hierarchical sub-FFT's first table is refused
//   plan-ip4-table-refused        FftPlan<f64>(65536): the IP4-AoS path's first table is refused
//   plan-four-step-table-refused  FftPlan<f64>(2^21): the four-step path's first table is refused
//   plan-four-step-subplan-refused  FftPlan<f64>(2^19): the four-step's first sub-plan is refused
//   bluestein-plan-refused        BluesteinPlan<f64>(7): its power-of-two plan is refused
//   nd-axis-plan-refused          NdFftPlan<f64>({8, 8}): the first axis plan is refused
//   nd-bluestein-axis-refused     NdFftPlan<f64>({6, 1000}): the first (Bluestein) axis plan is refused
//   sparse-plan-refused           SparseFftPlan<f64>(1024, 4): its bucket plan is refused
// The table modes refuse every request aligned to 32 bytes or more (the raw tables ask for 32 or 64; the containers
// the plan also holds ask for their element's alignment); the plan modes refuse exactly a plan object's request shape
// (refusing_allocator.hpp). Either way the refused request is the run's first.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/containers/span.hpp>
#include <crd/hesap/fft/bluestein.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/hesap/fft/nd_fft.hpp>
#include <crd/hesap/fft/sparse_fft.hpp>

#include <cstring>

namespace
{
namespace fft = crd::hesap::fft;
using crd_diag_fatal::RefusingAllocator;

constexpr crd::usize kRawTableAlignment = 32U; // the smallest alignment a plan's raw tables ask for

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// The FftPlan modes. Returns false for an unknown mode.
[[nodiscard]] bool run_plan_mode(const char* mode)
{
    crd::usize n = 0U;
    bool subplan = false;
    if (is(mode, "plan-hier-table-refused"))
    {
        n = 4096U;
    }
    else if (is(mode, "plan-ip4-table-refused"))
    {
        n = crd::usize{1} << 16U;
    }
    else if (is(mode, "plan-four-step-table-refused"))
    {
        n = crd::usize{1} << 21U;
    }
    else if (is(mode, "plan-four-step-subplan-refused"))
    {
        n = crd::usize{1} << 19U;
        subplan = true;
    }
    else
    {
        return false;
    }
    crd_diag_fatal::arm(mode, fft::detail::kFftPlanOutOfMemory);
    RefusingAllocator alloc("hesap-fft-fatal-refusing");
    if (subplan)
    {
        alloc.refuse_shape(sizeof(fft::FftPlan<crd::f64>), alignof(fft::FftPlan<crd::f64>));
    }
    else
    {
        alloc.refuse_aligned(kRawTableAlignment);
    }
    const fft::FftPlan<crd::f64> plan(&alloc, n);
    return true;
}

// The plans built on FftPlan. Returns false for an unknown mode.
[[nodiscard]] bool run_wrapper_mode(const char* mode)
{
    RefusingAllocator alloc("hesap-fft-fatal-refusing");
    if (is(mode, "bluestein-plan-refused"))
    {
        crd_diag_fatal::arm(mode, fft::detail::kBluesteinPlanOutOfMemory);
        alloc.refuse_shape(sizeof(fft::FftPlan<crd::f64>), alignof(fft::FftPlan<crd::f64>));
        const fft::BluesteinPlan<crd::f64> plan(&alloc, 7U);
        return true;
    }
    if (is(mode, "nd-axis-plan-refused"))
    {
        crd_diag_fatal::arm(mode, fft::detail::kNdFftPlanOutOfMemory);
        alloc.refuse_shape(sizeof(fft::FftPlan<crd::f64>), alignof(fft::FftPlan<crd::f64>));
        const crd::usize dims[2] = {8U, 8U};
        const fft::NdFftPlan<crd::f64> plan(&alloc, crd::containers::ConstSpan<crd::usize>{dims, 2U});
        return true;
    }
    if (is(mode, "nd-bluestein-axis-refused"))
    {
        crd_diag_fatal::arm(mode, fft::detail::kNdFftPlanOutOfMemory);
        // The second axis only sizes the plan's line buffer (16 bytes per element) far past any plan object: with a
        // largest axis of 6 the buffer asked for exactly a shipping BluesteinPlan's 128 bytes and was refused first.
        alloc.refuse_shape(sizeof(fft::BluesteinPlan<crd::f64>), alignof(fft::BluesteinPlan<crd::f64>));
        const crd::usize dims[2] = {6U, 1000U};
        const fft::NdFftPlan<crd::f64> plan(&alloc, crd::containers::ConstSpan<crd::usize>{dims, 2U});
        return true;
    }
    if (is(mode, "sparse-plan-refused"))
    {
        crd_diag_fatal::arm(mode, fft::detail::kSparseFftPlanOutOfMemory);
        alloc.refuse_shape(sizeof(fft::FftPlan<crd::f64>), alignof(fft::FftPlan<crd::f64>));
        const fft::SparseFftPlan<crd::f64> plan(&alloc, 1024U, 4U);
        return true;
    }
    return false;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    return run_plan_mode(mode) || run_wrapper_mode(mode);
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
