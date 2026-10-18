// hesap-dsp contract fatals (crd-hesap-dsp-fatal-* CTests, tests/numerics/hesap-dsp). The FFT convolver and the
// spectral estimators build their real-FFT plans in blocks of the caller's allocator; one that refuses a plan (a
// sub-budget allocator returns nullptr) leaves them out of memory, fatal in every build, never a null plan
// constructed into (DIAG.3a; the plan was constructed into the refused pointer before). One mode per run (argv[1]);
// see expected_fatal.hpp for the exit protocol:
//   convolver-plan-refused  FftConvolver<f64>(100, 30): its plan is refused
//   welch-plan-refused      welch_psd<f64>: the first per-job segment plan is refused
//   stft-plan-refused       stft<f64>: the first per-job segment plan is refused
// The allocator refuses exactly a plan object's request shape (refusing_allocator.hpp), so every other allocation is
// granted and the plan is the run's first refused one.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/containers/span.hpp>
#include <crd/hesap/dsp/convolution.hpp>
#include <crd/hesap/dsp/spectral.hpp>
#include <crd/hesap/fft/real_fft.hpp>
#include <crd/jobs/jobs.hpp>

#include <cstring>

namespace
{
namespace dsp = crd::hesap::dsp;
using crd_diag_fatal::RefusingAllocator;

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    const bool convolver = is(mode, "convolver-plan-refused");
    const bool welch = is(mode, "welch-plan-refused");
    const bool stft = is(mode, "stft-plan-refused");
    if (!convolver && !welch && !stft)
    {
        return false;
    }
    crd_diag_fatal::arm(mode, convolver ? dsp::detail::kFftConvolverOutOfMemory : dsp::detail::kSegmentPlanOutOfMemory);
    crd::jobs::init(crd::jobs::Config{.num_threads = 2});
    RefusingAllocator alloc("hesap-dsp-fatal-refusing");
    alloc.refuse_shape(sizeof(crd::hesap::fft::RealFftPlan<crd::f64>), alignof(crd::hesap::fft::RealFftPlan<crd::f64>));
    crd::f64 x[256] = {};
    for (crd::usize i = 0; i < 256U; ++i)
    {
        x[i] = static_cast<crd::f64>(i % 7U);
    }
    const crd::containers::ConstSpan<crd::f64> signal{x, 256U};
    if (convolver)
    {
        const dsp::FftConvolver<crd::f64> c(&alloc, 100U, 30U);
    }
    else if (welch)
    {
        (void)dsp::welch_psd<crd::f64>(&alloc, signal, 1.0, 64U);
    }
    else
    {
        crd::usize frames = 0U;
        crd::usize freqs = 0U;
        (void)dsp::stft<crd::f64>(&alloc, signal, 64U, 32U, frames, freqs);
    }
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
