// crd-hesap-dsp v11-d — special FIR designs: Savitzky-Golay (vs scipy) + raised-cosine/RRC (vs MATLAB rcosdesign).
// Both closed-form ⇒ coefficient match (savgol ~1e-12 vs scipy; rcos/rrc ~1e-10 vs MATLAB).

#include <crd/hesap/dsp/fir_special.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "special_refs.inc"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
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

TEST_CASE("dsp fir_special: Savitzky-Golay coeffs match scipy (closed-form => 1e-12)", "[v11-d][dsp][savgol]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    check(ref_savgol_5_2, dsp::savgol_coeffs<f64>(&alloc, 5, 2), 1e-12);
    check(ref_savgol_11_3, dsp::savgol_coeffs<f64>(&alloc, 11, 3), 1e-11);
    check(ref_savgol_21_4, dsp::savgol_coeffs<f64>(&alloc, 21, 4), 1e-10);
    check(ref_savgol_11_2_d1, dsp::savgol_coeffs<f64>(&alloc, 11, 2, 1), 1e-11);
    // smoothing coeffs sum to 1 (a deriv-0 SG filter preserves DC).
    const auto c = dsp::savgol_coeffs<f64>(&alloc, 11, 3);
    f64 s = 0.0;
    for (usize i = 0; i < c.size(); ++i)
    {
        s += c[i];
    }
    CHECK_THAT(s, WithinAbs(1.0, 1e-12));
}

TEST_CASE("dsp fir_special: root-raised-cosine + raised-cosine match MATLAB rcosdesign", "[v11-d][dsp][rrc]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    check(ref_rrc_025_6_4, dsp::root_raised_cosine<f64>(&alloc, 0.25, 6, 4), 1e-10);
    check(ref_rrc_05_8_4, dsp::root_raised_cosine<f64>(&alloc, 0.5, 8, 4), 1e-10);
    check(ref_rcos_025_6_4, dsp::raised_cosine<f64>(&alloc, 0.25, 6, 4), 1e-10);
    check(ref_rcos_05_8_4, dsp::raised_cosine<f64>(&alloc, 0.5, 8, 4), 1e-10);
    // RRC is unit-energy (MATLAB rcosdesign normalization).
    const auto h = dsp::root_raised_cosine<f64>(&alloc, 0.25, 6, 4);
    f64 e = 0.0;
    for (usize i = 0; i < h.size(); ++i)
    {
        e += h[i] * h[i];
    }
    CHECK_THAT(e, WithinAbs(1.0, 1e-12));
}
