// crd-hesap-dsp v11-j — convolution gates. direct convolve bit-exact; fftconvolve == direct (FFT roundoff) AND
// vs scipy.fftconvolve (~1e-10); correlate vs scipy.correlate.
#include <crd/hesap/dsp/convolution.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include "conv_refs.inc"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64; using crd::usize;
using Catch::Matchers::WithinAbs;

TEST_CASE("dsp conv: direct convolve == fftconvolve == scipy", "[v11-j][dsp][conv]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize na = sizeof(ref_conv_a)/sizeof(double);
    const usize nb = sizeof(ref_conv_b)/sizeof(double);
    const auto direct = dsp::convolve<f64>(&alloc, cont::ConstSpan<f64>(ref_conv_a,na), cont::ConstSpan<f64>(ref_conv_b,nb));
    const auto fftc = dsp::fftconvolve<f64>(&alloc, cont::ConstSpan<f64>(ref_conv_a,na), cont::ConstSpan<f64>(ref_conv_b,nb));
    REQUIRE(direct.size() == na+nb-1);
    REQUIRE(fftc.size() == na+nb-1);
    for (usize i = 0; i < direct.size(); ++i)
    {
        INFO("[" << i << "]");
        CHECK_THAT(direct[i], WithinAbs(ref_conv_full[i], 1e-10)); // direct vs scipy
        CHECK_THAT(fftc[i], WithinAbs(ref_conv_full[i], 1e-9));    // FFT-based vs scipy
        CHECK_THAT(fftc[i], WithinAbs(direct[i], 1e-9));           // FFT == direct
    }
}

TEST_CASE("dsp conv: correlate matches scipy.signal.correlate", "[v11-j][dsp][conv]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize na = sizeof(ref_conv_a)/sizeof(double);
    const usize nb = sizeof(ref_conv_b)/sizeof(double);
    const auto corr = dsp::correlate<f64>(&alloc, cont::ConstSpan<f64>(ref_conv_a,na), cont::ConstSpan<f64>(ref_conv_b,nb));
    REQUIRE(corr.size() == na+nb-1);
    for (usize i = 0; i < corr.size(); ++i) { INFO("[" << i << "]"); CHECK_THAT(corr[i], WithinAbs(ref_corr_full[i], 1e-10)); }
}

TEST_CASE("dsp conv: FftConvolver (plan-cached, reusable) == direct == scipy", "[v11-j][dsp][conv]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize na = sizeof(ref_conv_a) / sizeof(double);
    const usize nb = sizeof(ref_conv_b) / sizeof(double);
    dsp::FftConvolver<f64> conv(&alloc, na, nb);
    const auto y1 = conv.convolve(cont::ConstSpan<f64>(ref_conv_a, na), cont::ConstSpan<f64>(ref_conv_b, nb));
    // reuse the SAME convolver a second time (plan cached) — must give the identical result.
    const auto y2 = conv.convolve(cont::ConstSpan<f64>(ref_conv_a, na), cont::ConstSpan<f64>(ref_conv_b, nb));
    REQUIRE(y1.size() == na + nb - 1);
    for (usize i = 0; i < y1.size(); ++i)
    {
        INFO("[" << i << "]");
        CHECK_THAT(y1[i], WithinAbs(ref_conv_full[i], 1e-9)); // vs scipy
        CHECK(y2[i] == y1[i]);                                // plan reuse is deterministic
    }
}

TEST_CASE("dsp conv: oaconvolve == scipy + deconvolve recovers quotient", "[v11-j][dsp][conv]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize na = sizeof(ref_oa_a)/sizeof(double);
    const usize nb = sizeof(ref_oa_b)/sizeof(double);
    const auto oa = dsp::oaconvolve<f64>(&alloc, cont::ConstSpan<f64>(ref_oa_a,na), cont::ConstSpan<f64>(ref_oa_b,nb));
    REQUIRE(oa.size() == na+nb-1);
    for (usize i = 0; i < oa.size(); ++i)
    {
        INFO("oa[" << i << "]");
        CHECK_THAT(oa[i], WithinAbs(ref_oa_full[i], 1e-9)); // overlap-add == scipy fftconvolve
    }
    // deconvolve: recover the quotient.
    const usize ns = sizeof(ref_deconv_sig)/sizeof(double);
    const usize nd = sizeof(ref_deconv_div)/sizeof(double);
    const usize nq = sizeof(ref_deconv_q)/sizeof(double);
    cont::Array<f64> rem(&alloc);
    const auto q = dsp::deconvolve<f64>(&alloc, cont::ConstSpan<f64>(ref_deconv_sig,ns), cont::ConstSpan<f64>(ref_deconv_div,nd), rem);
    REQUIRE(q.size() == nq);
    for (usize i = 0; i < nq; ++i) { INFO("q[" << i << "]"); CHECK_THAT(q[i], WithinAbs(ref_deconv_q[i], 1e-10)); }
}
