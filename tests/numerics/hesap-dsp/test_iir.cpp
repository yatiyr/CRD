// crd-hesap-dsp v11-e — IIR design gates. Analog prototypes (Butterworth/Cheb-I/Cheb-II) closed-form ⇒ pole
// match vs scipy. Digital designs (butter/cheby1/cheby2 → zpk → SOS) gated on RESPONSE-EQUALITY vs scipy's sos
// (order-independent: |H(e^jw)| at 64 points) + spec-compliance (monotonic / equiripple as appropriate).

#include <crd/hesap/dsp/filter.hpp>
#include <crd/hesap/dsp/freqz.hpp>
#include <crd/hesap/dsp/iir.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "iir_refs.inc"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cmath>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using crd::hesap::Complex;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
struct PoleKey
{
    f64 re, im;
};

// compare a set of complex poles (sorted by re,im) to a flat scipy reference C array (interleaved re,im).
template <crd::usize N> void check_poles(const double (&ref)[N], const dsp::Zpk<f64>& zpk, double tol)
{
    cont::Array<PoleKey> ps(zpk.p.allocator());
    for (usize i = 0; i < zpk.p.size(); ++i)
    {
        ps.push_back(PoleKey{zpk.p[i].re, zpk.p[i].im});
    }
    auto key = [](double v) { return std::round(v * 1e9) / 1e9; }; // match scipy's round-to-1e-9 sort key
    std::sort(ps.data(), ps.data() + ps.size(),
              [&](const PoleKey& a, const PoleKey& b)
              { return key(a.re) != key(b.re) ? key(a.re) < key(b.re) : key(a.im) < key(b.im); });
    REQUIRE(ps.size() * 2 == N);
    for (usize i = 0; i < ps.size(); ++i)
    {
        INFO("pole[" << i << "]");
        CHECK_THAT(ps[i].re, WithinAbs(ref[2 * i], tol));
        CHECK_THAT(ps[i].im, WithinAbs(ref[2 * i + 1], tol));
    }
}

// design via zpk -> sos, evaluate |H| at 64 points, compare to scipy's sosfreqz magnitude (order-independent).
template <crd::usize N>
void check_response(const double (&ref)[N], crd::memory::IAllocator* a, const dsp::Zpk<f64>& zpk)
{
    const auto sos = dsp::zpk_to_sos<f64>(a, zpk);
    cont::Array<f64> w(a);
    cont::Array<Complex<f64>> h(a);
    dsp::sosfreqz<f64>(sos, 64, w, h);
    REQUIRE(N == 64);
    for (usize i = 0; i < 64; ++i)
    {
        INFO("mag[" << i << "]");
        CHECK_THAT(std::hypot(h[i].re, h[i].im), WithinAbs(ref[i], 1e-9));
    }
}
} // namespace

TEST_CASE("dsp iir: analog prototypes (Butterworth/Cheb-I/Cheb-II) match scipy poles", "[v11-e][dsp][iir]")
{
    crd::memory::TlsfAllocator alloc(1U << 21);
    check_poles(ref_buttap4_poles, dsp::buttap<f64>(&alloc, 4), 1e-12);
    check_poles(ref_cheb1ap4_poles, dsp::cheb1ap<f64>(&alloc, 4, 1.0), 1e-10);
    check_poles(ref_cheb2ap4_poles, dsp::cheb2ap<f64>(&alloc, 4, 40.0), 1e-10);
}

TEST_CASE("dsp iir: digital butter/cheby1/cheby2 -- response matches scipy sos (order-independent)", "[v11-e][dsp][iir]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    check_response(ref_butter6_mag, &alloc, dsp::butter<f64>(&alloc, 6, 0.3));
    check_response(ref_cheby1_6_mag, &alloc, dsp::cheby1<f64>(&alloc, 6, 1.0, 0.3));
    check_response(ref_cheby2_6_mag, &alloc, dsp::cheby2<f64>(&alloc, 6, 40.0, 0.3));
}

TEST_CASE("dsp iir: Butterworth meets its spec (monotonic, -3 dB at cutoff)", "[v11-e][dsp][iir]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto zpk = dsp::butter<f64>(&alloc, 6, 0.3);
    const auto sos = dsp::zpk_to_sos<f64>(&alloc, zpk);
    cont::Array<f64> w(&alloc);
    cont::Array<Complex<f64>> h(&alloc);
    dsp::sosfreqz<f64>(sos, 1024, w, h);
    const f64 pi = std::numbers::pi_v<f64>;
    f64 prev = 2.0;
    usize cut_bin = 0;
    f64 cut_dist = 1.0;
    for (usize i = 0; i < 1024; ++i)
    {
        const f64 mag = std::hypot(h[i].re, h[i].im);
        CHECK(mag <= prev + 1e-9); // monotonically NON-increasing (Butterworth maximally flat)
        prev = mag;
        const f64 d = std::abs(w[i] / (2.0 * pi) - 0.15); // cutoff Wn=0.3 ⇒ 0.15 cycles/sample
        if (d < cut_dist)
        {
            cut_dist = d;
            cut_bin = i;
        }
    }
    // -3 dB at the cutoff: the single closest bin to 0.15 (a steep 6th-order slope ⇒ a small bin-offset tolerance).
    CHECK_THAT(std::hypot(h[cut_bin].re, h[cut_bin].im), WithinAbs(1.0 / std::sqrt(2.0), 0.01));
}

TEST_CASE("dsp iir: Bessel analog prototype + digital match scipy (delay-normalized)", "[v11-e][dsp][iir]")
{
    crd::memory::TlsfAllocator alloc(1U << 21);
    check_poles(ref_besselap4_poles, dsp::besselap<f64>(&alloc, 4), 1e-9);
    const auto zpk = dsp::besselap<f64>(&alloc, 4);
    CHECK_THAT(zpk.k, WithinRel(ref_besselap4_k, 1e-9));
    // digital bessel response vs scipy.
    check_response(ref_bessel6_mag, &alloc, dsp::bessel<f64>(&alloc, 6, 0.3));
}
