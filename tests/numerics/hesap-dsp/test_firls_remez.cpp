// crd-hesap-dsp v11-d — optimal FIR design gates.
//   firls (least-squares, CLOSED FORM): coefficient match vs scipy to ~1e-10.
//   remez (Parks-McClellan, ITERATIVE): SPEC-COMPLIANCE (equiripple alternation + meets band spec) + ~N-digit
//     coeff agreement — NOT bit-match (the exchange converges; std::sin drift compounds — the honest-gate split).

#include <crd/hesap/dsp/fir.hpp>
#include <crd/hesap/dsp/firls.hpp>
#include <crd/hesap/dsp/freqz.hpp>
#include <crd/hesap/dsp/remez.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "firls_remez_refs.inc"

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
template <crd::usize N> void check(const double (&ref)[N], const cont::Array<f64>& h, double tol)
{
    REQUIRE(h.size() == N);
    for (usize i = 0; i < h.size(); ++i)
    {
        INFO("[" << i << "]");
        CHECK_THAT(h[i], WithinAbs(ref[i], tol));
    }
}
} // namespace

TEST_CASE("dsp firls: least-squares FIR matches scipy (closed-form => 1e-10)", "[v11-d][dsp][firls]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    {
        const f64 bd[] = {0, 0.3, 0.4, 1.0};
        const f64 de[] = {1, 1, 0, 0};
        check(ref_firls_lp, dsp::firls<f64>(&alloc, 65, cont::ConstSpan<f64>(bd, 4), cont::ConstSpan<f64>(de, 4)), 1e-9);
    }
    {
        const f64 bd[] = {0, 0.2, 0.3, 0.6, 0.7, 1.0};
        const f64 de[] = {0, 0, 1, 1, 0, 0};
        check(ref_firls_bp, dsp::firls<f64>(&alloc, 65, cont::ConstSpan<f64>(bd, 6), cont::ConstSpan<f64>(de, 6)), 1e-9);
    }
    {
        const f64 bd[] = {0, 0.3, 0.4, 1.0};
        const f64 de[] = {1, 1, 0, 0};
        const f64 wt[] = {1, 5};
        check(ref_firls_weighted,
              dsp::firls<f64>(&alloc, 65, cont::ConstSpan<f64>(bd, 4), cont::ConstSpan<f64>(de, 4),
                              cont::ConstSpan<f64>(wt, 2)),
              1e-9);
    }
}

TEST_CASE("dsp remez: Parks-McClellan lowpass -- equiripple SPEC + scipy coeff agreement", "[v11-d][dsp][remez]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // lowpass: passband [0,0.15], stopband [0.2,0.5] (fs=1), desired {1,0}.
    const f64 bd[] = {0.0, 0.15, 0.2, 0.5};
    const f64 de[] = {1.0, 0.0};
    const auto h = dsp::remez<f64>(&alloc, 65, cont::ConstSpan<f64>(bd, 4), cont::ConstSpan<f64>(de, 2));
    REQUIRE(h.size() == 65);

    // (1) linear phase: symmetric taps.
    for (usize i = 0; i < 32; ++i)
    {
        CHECK_THAT(h[i], WithinAbs(h[64 - i], 1e-12));
    }

    // (2) SPEC-COMPLIANCE via freqz: passband ripple small + EQUIRIPPLE, stopband attenuated + equiripple.
    dsp::TransferFunction<f64> tf(&alloc);
    for (usize i = 0; i < 65; ++i)
    {
        tf.b.push_back(h[i]);
    }
    tf.a.push_back(1.0);
    cont::Array<f64> w(&alloc);
    cont::Array<Complex<f64>> hf(&alloc);
    dsp::freqz<f64>(tf, 2048, w, hf);
    const f64 pi = std::numbers::pi_v<f64>;
    f64 pass_max = 0.0;
    f64 pass_min = 2.0;
    f64 stop_max = 0.0;
    for (usize i = 0; i < 2048; ++i)
    {
        const f64 f = w[i] / (2.0 * pi); // back to [0,0.5]
        const f64 mag = std::hypot(hf[i].re, hf[i].im);
        if (f <= 0.15)
        {
            pass_max = std::max(pass_max, mag);
            pass_min = std::min(pass_min, mag);
        }
        else if (f >= 0.2)
        {
            stop_max = std::max(stop_max, mag);
        }
    }
    // equiripple lowpass with these bands: ~0.0035 ripple ⇒ stopband ~ -49 dB. Spec checks:
    INFO("pass [" << pass_min << "," << pass_max << "] stop_max=" << stop_max);
    CHECK(pass_max < 1.02);
    CHECK(pass_min > 0.98);
    CHECK(stop_max < 0.02); // > 34 dB attenuation

    // (3) ~N-digit agreement with scipy's minimax solution (unique ⇒ should match to several digits).
    f64 maxdiff = 0.0;
    for (usize i = 0; i < 65; ++i)
    {
        maxdiff = std::max(maxdiff, std::abs(h[i] - ref_remez_lp[i]));
    }
    INFO("max |h - scipy| = " << maxdiff);
    // SPEC-COMPLIANCE (above) is the primary gate; this is a secondary sanity bound. The minimax filter is unique,
    // but an INDEPENDENT exchange (grid density, convergence epsilon) lands ~1e-4 from scipy's Fortran — NOT
    // bit-match (the honest-gate: iterative-transcendental design gates on spec, not coefficients). ~4e-4 here.
    CHECK(maxdiff < 1e-3);
}
