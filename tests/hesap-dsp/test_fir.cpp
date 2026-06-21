// crd-hesap-dsp v11-c — FIR window-method design gates. firwin is closed-form (sinc x window) so it gets BOTH:
//   (1) coefficient match vs scipy.signal.firwin to ~1e-12 (fir_refs.inc), AND
//   (2) spec-compliance: passband gain ~1, stopband attenuation, linear phase (symmetric taps).

#include <crd/hesap/dsp/fir.hpp>
#include <crd/hesap/dsp/freqz.hpp>
#include <crd/hesap/dsp/windows.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "fir_refs.inc"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using crd::hesap::Complex;
using Catch::Matchers::WithinAbs;

namespace
{
template <crd::usize N> void check_taps(const double (&ref)[N], const cont::Array<f64>& h, double tol = 1e-12)
{
    REQUIRE(h.size() == N);
    for (usize i = 0; i < h.size(); ++i)
    {
        INFO("[" << i << "]");
        CHECK_THAT(h[i], WithinAbs(ref[i], tol));
    }
}
} // namespace

TEST_CASE("dsp fir: firwin coefficients match scipy (closed-form => 1e-12)", "[v11-c][dsp][fir]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const f64 c1[] = {0.3};
    const f64 c1b[] = {0.4};
    const f64 c2[] = {0.2, 0.5};
    check_taps(ref_lowpass_0_3, dsp::firwin<f64>(&alloc, 35, cont::ConstSpan<f64>(c1, 1),
                                               cont::ConstSpan<f64>(dsp::hamming<f64>(&alloc, 35).data(), 35), true));
    check_taps(ref_highpass_0_4,
               dsp::firwin<f64>(&alloc, 35, cont::ConstSpan<f64>(c1b, 1),
                                cont::ConstSpan<f64>(dsp::hamming<f64>(&alloc, 35).data(), 35), false));
    check_taps(ref_bandpass_0_2_0_5,
               dsp::firwin<f64>(&alloc, 35, cont::ConstSpan<f64>(c2, 2),
                                cont::ConstSpan<f64>(dsp::hamming<f64>(&alloc, 35).data(), 35), false));
    check_taps(ref_bandstop_0_2_0_5,
               dsp::firwin<f64>(&alloc, 35, cont::ConstSpan<f64>(c2, 2),
                                cont::ConstSpan<f64>(dsp::hamming<f64>(&alloc, 35).data(), 35), true));
    const f64 c3[] = {0.25};
    check_taps(ref_lowpass_hann, dsp::firwin<f64>(&alloc, 31, cont::ConstSpan<f64>(c3, 1),
                                                cont::ConstSpan<f64>(dsp::hann<f64>(&alloc, 31).data(), 31), true));
    check_taps(ref_lowpass_blackman,
               dsp::firwin<f64>(&alloc, 31, cont::ConstSpan<f64>(c3, 1),
                                cont::ConstSpan<f64>(dsp::blackman<f64>(&alloc, 31).data(), 31), true));
    check_taps(ref_lowpass_noscale,
               dsp::firwin<f64>(&alloc, 31, cont::ConstSpan<f64>(c3, 1),
                                cont::ConstSpan<f64>(dsp::hamming<f64>(&alloc, 31).data(), 31), true, false));
}

TEST_CASE("dsp fir: firwin2 (frequency sampling) matches scipy", "[v11-c][dsp][fir]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    {
        const f64 f[] = {0.0, 0.5, 1.0};
        const f64 g[] = {1.0, 1.0, 0.0};
        check_taps(ref_firwin2_lp,
                   dsp::firwin2<f64>(&alloc, 65, cont::ConstSpan<f64>(f, 3), cont::ConstSpan<f64>(g, 3),
                                     cont::ConstSpan<f64>(dsp::hamming<f64>(&alloc, 65).data(), 65)),
                   1e-11);
    }
    {
        const f64 f[] = {0.0, 0.2, 0.4, 0.6, 1.0};
        const f64 g[] = {0.0, 0.0, 1.0, 0.0, 0.0};
        check_taps(ref_firwin2_bp,
                   dsp::firwin2<f64>(&alloc, 65, cont::ConstSpan<f64>(f, 5), cont::ConstSpan<f64>(g, 5),
                                     cont::ConstSpan<f64>(dsp::hamming<f64>(&alloc, 65).data(), 65)),
                   1e-11);
    }
    {
        const f64 f[] = {0.0, 0.3, 0.3, 1.0};
        const f64 g[] = {1.0, 1.0, 0.5, 0.5};
        check_taps(ref_firwin2_shelf,
                   dsp::firwin2<f64>(&alloc, 65, cont::ConstSpan<f64>(f, 4), cont::ConstSpan<f64>(g, 4),
                                     cont::ConstSpan<f64>(dsp::hamming<f64>(&alloc, 65).data(), 65)),
                   1e-11);
    }
}

TEST_CASE("dsp fir: firwin lowpass meets its SPEC (passband ~1, stopband attenuated, linear phase)", "[v11-c][dsp][fir]")
{
    crd::memory::TlsfAllocator alloc(1U << 21);
    const usize ntaps = 65;
    const f64 fc[] = {0.3}; // cutoff at 0.3 * Nyquist
    const auto h = dsp::firwin<f64>(&alloc, ntaps, cont::ConstSpan<f64>(fc, 1),
                                    cont::ConstSpan<f64>(dsp::hamming<f64>(&alloc, ntaps).data(), ntaps), true);
    // linear phase ⇒ symmetric taps.
    for (usize i = 0; i < ntaps / 2; ++i)
    {
        CHECK_THAT(h[i], WithinAbs(h[ntaps - 1 - i], 1e-14));
    }
    // build the tf (a = {1}) and evaluate freqz.
    dsp::TransferFunction<f64> tf(&alloc);
    for (usize i = 0; i < ntaps; ++i)
    {
        tf.b.push_back(h[i]);
    }
    tf.a.push_back(1.0);
    cont::Array<f64> w(&alloc);
    cont::Array<Complex<f64>> hf(&alloc);
    dsp::freqz<f64>(tf, 512, w, hf);
    // passband (w < 0.25*pi): gain ~1 (hamming ripple ~0.002). stopband (w > 0.4*pi): attenuated > 40 dB.
    const f64 cutoff_w = 0.3 * std::numbers::pi_v<f64>;
    for (usize i = 0; i < 512; ++i)
    {
        const f64 mag = std::hypot(hf[i].re, hf[i].im);
        if (w[i] < 0.6 * cutoff_w) // well inside passband
        {
            CHECK_THAT(mag, WithinAbs(1.0, 0.02));
        }
        else if (w[i] > 1.7 * cutoff_w) // well inside stopband
        {
            CHECK(mag < 0.01); // > 40 dB down (hamming gives ~53 dB)
        }
    }
}
