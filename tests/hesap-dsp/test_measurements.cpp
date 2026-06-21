// crd-hesap-dsp v11-s — detection + measurements. find_peaks/prominences/widths/argrelextrema/detrend vs scipy
// (exact / 1e-9); the spectrum metrics (THD/SNR/SINAD/SFDR/ENOB) gated analytically with a planted harmonic;
// rms/crest analytic; cspline1d via the reconstruction property.

#include <crd/hesap/dsp/measurements.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "measurements_refs.inc"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numbers>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr f64 kPi = std::numbers::pi_v<f64>;

cont::Array<f64> sig(crd::memory::IAllocator* a, usize n)
{
    cont::Array<f64> x(a);
    x.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        const f64 fi = static_cast<f64>(i);
        x[i] = std::sin(2 * kPi * 0.05 * fi) + 0.4 * std::sin(2 * kPi * 0.21 * fi) + 0.2 * std::cos(2 * kPi * 0.017 * fi);
    }
    return x;
}

template <usize N> void check_idx(const unsigned long (&ref)[N], usize ref_n, const cont::Array<usize>& got)
{
    REQUIRE(got.size() == ref_n);
    for (usize i = 0; i < ref_n; ++i)
    {
        CHECK(got[i] == static_cast<usize>(ref[i]));
    }
}
template <usize N> void check_arr(const double (&ref)[N], const cont::Array<f64>& got, double tol)
{
    REQUIRE(got.size() == N);
    for (usize i = 0; i < N; ++i)
    {
        INFO("i=" << i);
        CHECK_THAT(got[i], WithinAbs(ref[i], tol));
    }
}
} // namespace

TEST_CASE("dsp measurements: find_peaks + prominences + widths match scipy", "[v11-s][dsp][measure]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto x = sig(&alloc, 240);
    const cont::ConstSpan<f64> xs(x.data(), 240);
    const auto peaks = dsp::find_peaks<f64>(&alloc, xs);
    check_idx(ref_fp_peaks, ref_fp_peaks_n, peaks);
    const auto prom = dsp::peak_prominences<f64>(&alloc, xs, cont::ConstSpan<usize>(peaks.data(), peaks.size()));
    check_arr(ref_fp_prom, prom.prominences, 1e-9);
    const auto widths = dsp::peak_widths<f64>(&alloc, xs, cont::ConstSpan<usize>(peaks.data(), peaks.size()), 0.5, prom);
    check_arr(ref_fp_widths, widths, 1e-9);
    // height + distance filter
    const auto pk2 = dsp::find_peaks<f64>(&alloc, xs, 0.3, 10);
    check_idx(ref_fp_hd, ref_fp_hd_n, pk2);
}

TEST_CASE("dsp measurements: parallel local_maxima == serial (determinism moat)", "[v11-s][dsp][measure]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize n = 40000; // large enough to trigger the parallel path
    cont::Array<f64> x(&alloc);
    x.resize(n);
    crd::u64 s = 7ULL;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const f64 e = (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
        x[i] = std::sin(0.05 * static_cast<f64>(i)) + 0.4 * e;
    }
    const cont::ConstSpan<f64> xs(x.data(), n);
    const auto par = dsp::local_maxima_mt<f64>(&alloc, xs);
    const auto ser = dsp::local_maxima<f64>(&alloc, xs);
    REQUIRE(par.size() == ser.size());
    for (usize i = 0; i < ser.size(); ++i)
    {
        CHECK(par[i] == ser[i]); // bit-identical to the serial scan, any thread count
    }
}

TEST_CASE("dsp measurements: argrelextrema + detrend match scipy", "[v11-s][dsp][measure]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto x = sig(&alloc, 240);
    const auto amax = dsp::argrelextrema<f64>(&alloc, cont::ConstSpan<f64>(x.data(), 240), true, 2);
    check_idx(ref_argrel_max, ref_argrel_max_n, amax);
    cont::Array<f64> xt(&alloc);
    xt.resize(120);
    for (usize i = 0; i < 120; ++i)
    {
        xt[i] = 0.5 + 0.02 * static_cast<f64>(i) + std::sin(2 * kPi * 0.1 * static_cast<f64>(i));
    }
    const cont::ConstSpan<f64> xts(xt.data(), 120);
    check_arr(ref_detrend_const, dsp::detrend<f64>(&alloc, xts, false), 1e-9);
    check_arr(ref_detrend_linear, dsp::detrend<f64>(&alloc, xts, true), 1e-9);
}

TEST_CASE("dsp measurements: THD/SNR/SINAD/SFDR/ENOB on a planted harmonic", "[v11-s][dsp][measure]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize n = 256;
    cont::Array<f64> x(&alloc);
    x.resize(n);
    for (usize i = 0; i < n; ++i) // fundamental (8 cycles) + a 3rd harmonic at 0.01 (= -40 dB), integer cycles ⇒ no leak
    {
        const f64 fi = static_cast<f64>(i);
        x[i] = std::cos(2 * kPi * 8.0 * fi / n) + 0.01 * std::cos(2 * kPi * 24.0 * fi / n);
    }
    const cont::ConstSpan<f64> xs(x.data(), n);
    CHECK_THAT(dsp::thd<f64>(&alloc, xs), WithinAbs(-40.0, 0.3));   // 20·log10(0.01)
    CHECK_THAT(dsp::sinad<f64>(&alloc, xs), WithinAbs(40.0, 0.3));  // fundamental / harmonic
    CHECK_THAT(dsp::sfdr<f64>(&alloc, xs), WithinAbs(40.0, 0.3));   // fundamental / largest spur
    CHECK_THAT(dsp::enob<f64>(&alloc, xs), WithinAbs((40.0 - 1.76) / 6.02, 0.1));
    // a clean tone: very low THD, very high SNR.
    for (usize i = 0; i < n; ++i)
    {
        x[i] = std::cos(2 * kPi * 8.0 * static_cast<f64>(i) / n);
    }
    CHECK(dsp::thd<f64>(&alloc, xs) < -60.0);
    CHECK(dsp::snr<f64>(&alloc, xs) > 60.0);
}

TEST_CASE("dsp measurements: rms / crest / cspline1d", "[v11-s][dsp][measure]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize n = 400;
    cont::Array<f64> x(&alloc);
    x.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        x[i] = std::sin(2 * kPi * 5.0 * static_cast<f64>(i) / n);
    }
    const cont::ConstSpan<f64> xs(x.data(), n);
    CHECK_THAT(dsp::rms<f64>(xs), WithinAbs(1.0 / std::sqrt(2.0), 1e-3));   // unit sine ⇒ RMS = 1/√2
    CHECK_THAT(dsp::crest_factor<f64>(xs), WithinAbs(std::sqrt(2.0), 2e-3)); // peak/RMS = √2
    // cubic B-spline: reconstruction (c[k-1]+4c[k]+c[k+1])/6 == signal at interior knots.
    cont::Array<f64> s(&alloc);
    s.resize(50);
    for (usize i = 0; i < 50; ++i)
    {
        s[i] = std::sin(0.3 * static_cast<f64>(i)) + 0.1 * static_cast<f64>(i);
    }
    const auto c = dsp::cspline1d<f64>(&alloc, cont::ConstSpan<f64>(s.data(), 50));
    for (usize k = 1; k + 1 < 50; ++k)
    {
        INFO("k=" << k);
        CHECK_THAT((c[k - 1] + 4 * c[k] + c[k + 1]) / 6.0, WithinAbs(s[k], 1e-9));
    }
}
