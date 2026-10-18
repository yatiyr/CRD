// hesap-wavelet contract fatals (crd-hesap-wavelet-fatal-* CTests, tests/numerics/hesap-wavelet). The continuous
// wavelet transform builds one FFT plan per job in blocks of the caller's allocator; one that refuses a plan (a
// sub-budget allocator returns nullptr) leaves the transform out of memory, fatal in every build, never a null plan
// constructed into (DIAG.3a; the plan was constructed into the refused pointer before). One mode per run (argv[1]);
// see expected_fatal.hpp for the exit protocol:
//   cwt-plan-refused  cwt<f64> with the morl wavelet: the first per-job scale plan is refused
// The allocator refuses exactly a plan object's request shape (refusing_allocator.hpp); the transform's data plan is a
// local object, so the first refused request is a per-job plan.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/hesap/wavelet/cwt.hpp>
#include <crd/jobs/jobs.hpp>

#include <cstring>

namespace
{
namespace wv = crd::hesap::wavelet;

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    if (std::strcmp(mode, "cwt-plan-refused") != 0)
    {
        return false;
    }
    crd_diag_fatal::arm(mode, wv::detail::kCwtPlanOutOfMemory);
    crd::jobs::init(crd::jobs::Config{.num_threads = 2});
    crd_diag_fatal::RefusingAllocator alloc("hesap-wavelet-fatal-refusing");
    alloc.refuse_shape(sizeof(crd::hesap::fft::FftPlan<crd::f64>), alignof(crd::hesap::fft::FftPlan<crd::f64>));
    crd::f64 x[128] = {};
    for (crd::usize i = 0; i < 128U; ++i)
    {
        x[i] = static_cast<crd::f64>(i % 5U);
    }
    const crd::f64 scales[4] = {1.0, 2.0, 3.0, 4.0};
    const wv::ContinuousWavelet w = wv::continuous_wavelet(crd::containers::StringView{"morl", 4U});
    (void)wv::cwt<crd::f64>(&alloc, crd::containers::ConstSpan<crd::f64>{x, 128U},
                            crd::containers::ConstSpan<crd::f64>{scales, 4U}, w);
    crd::jobs::shutdown();
    return true;
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
