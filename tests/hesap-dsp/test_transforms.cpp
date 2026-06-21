// crd-hesap-dsp v11-q — transforms. Self-contained gates: Goertzel vs the direct DFT bin; CZT(default params) == DFT
// and a zoom arc vs the direct nonuniform sum; rceps recovers an echo delay; fwht is self-inverse + a known case.

#include <crd/hesap/dsp/transforms.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numbers>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64;
using crd::u64;
using crd::usize;
using crd::hesap::Complex;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr f64 kPi = std::numbers::pi_v<f64>;

// direct DFT value at normalized frequency f (cycles/sample).
Complex<f64> dft_at(cont::ConstSpan<f64> x, f64 f)
{
    f64 re = 0, im = 0;
    for (usize n = 0; n < x.size(); ++n)
    {
        const f64 w = -2.0 * kPi * f * static_cast<f64>(n);
        re += x[n] * std::cos(w);
        im += x[n] * std::sin(w);
    }
    return Complex<f64>{re, im};
}

cont::Array<f64> noise(crd::memory::IAllocator* a, usize n, u64 seed)
{
    cont::Array<f64> x(a);
    x.resize(n);
    u64 s = seed;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        x[i] = (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
    }
    return x;
}
} // namespace

TEST_CASE("dsp transforms: Goertzel == the direct DFT bin", "[v11-q][dsp][transforms]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    const auto x = noise(&alloc, 64, 7ULL);
    const cont::ConstSpan<f64> xs(x.data(), 64);
    for (usize k : {3u, 5u, 11u})
    {
        const f64 f = static_cast<f64>(k) / 64.0;
        const auto g = dsp::goertzel<f64>(xs, f);
        const auto ref = dft_at(xs, f);
        INFO("bin " << k);
        CHECK_THAT(g.re, WithinAbs(ref.re, 1e-9));
        CHECK_THAT(g.im, WithinAbs(ref.im, 1e-9));
    }
}

TEST_CASE("dsp transforms: CZT default == DFT, zoom arc == direct sum", "[v11-q][dsp][transforms]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto x = noise(&alloc, 32, 13ULL);
    const cont::ConstSpan<f64> xs(x.data(), 32);
    // default params (theta_w = -2π/m, theta_a = 0) ⇒ the 32-point DFT.
    const auto X = dsp::czt<f64>(&alloc, xs, 32, -2.0 * kPi / 32.0, 0.0);
    for (usize k = 0; k < 32; ++k)
    {
        const auto ref = dft_at(xs, static_cast<f64>(k) / 32.0);
        INFO("dft bin " << k);
        CHECK_THAT(X[k].re, WithinAbs(ref.re, 1e-9));
        CHECK_THAT(X[k].im, WithinAbs(ref.im, 1e-9));
    }
    // zoom: M=24 points from f0=0.1, step df=0.005 ⇒ theta_a = 2π f0, theta_w = -2π df.
    const f64 f0 = 0.1, df = 0.005;
    const usize m = 24;
    const auto Z = dsp::czt<f64>(&alloc, xs, m, -2.0 * kPi * df, 2.0 * kPi * f0);
    for (usize k = 0; k < m; ++k)
    {
        const auto ref = dft_at(xs, f0 + static_cast<f64>(k) * df);
        INFO("zoom bin " << k);
        CHECK_THAT(Z[k].re, WithinAbs(ref.re, 1e-8));
        CHECK_THAT(Z[k].im, WithinAbs(ref.im, 1e-8));
    }
}

TEST_CASE("dsp transforms: rceps recovers an echo delay", "[v11-q][dsp][transforms]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize n = 512, delay = 40;
    cont::Array<f64> s(&alloc);
    s.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        s[i] = 0.0;
    }
    // a minimum-phase-ish decaying pulse + a delayed echo at `delay`.
    for (usize i = 0; i < 30; ++i)
    {
        s[i] = std::exp(-0.2 * static_cast<f64>(i)) * std::cos(0.5 * static_cast<f64>(i));
    }
    for (usize i = 0; i + delay < n; ++i)
    {
        s[i + delay] += 0.6 * s[i];
    }
    const auto c = dsp::rceps<f64>(&alloc, cont::ConstSpan<f64>(s.data(), n));
    // the cepstrum should have a prominent peak at the echo quefrency `delay` (ignore the low-quefrency region).
    usize peak = 10;
    for (usize q = 10; q < n / 2; ++q)
    {
        if (std::abs(c[q]) > std::abs(c[peak]))
        {
            peak = q;
        }
    }
    CHECK(peak == delay);
}

TEST_CASE("dsp transforms: impz + residuez + lifter", "[v11-q][dsp][transforms]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    // H(z) = 1/(1 - 0.5 z⁻¹) ⇒ h[n] = 0.5ⁿ.
    const f64 b[] = {1.0};
    const f64 a[] = {1.0, -0.5};
    const auto h = dsp::impz<f64>(&alloc, cont::ConstSpan<f64>(b, 1), cont::ConstSpan<f64>(a, 2), 20);
    for (usize n = 0; n < 20; ++n)
    {
        INFO("impz n=" << n);
        CHECK_THAT(h[n], WithinAbs(std::pow(0.5, static_cast<f64>(n)), 1e-12));
    }
    // residuez: single pole 0.5, residue 1 ⇒ reconstruct h[n] = Σ r_i p_iⁿ.
    cont::Array<Complex<f64>> poles(&alloc), res(&alloc);
    dsp::residuez<f64>(&alloc, cont::ConstSpan<f64>(b, 1), cont::ConstSpan<f64>(a, 2), poles, res);
    REQUIRE(poles.size() == 1);
    CHECK_THAT(poles[0].re, WithinAbs(0.5, 1e-10));
    for (usize n = 0; n < 10; ++n)
    {
        f64 re = 0.0;
        for (usize i = 0; i < poles.size(); ++i) // Σ r_i p_iⁿ
        {
            f64 pr = 1.0, pim = 0.0;
            for (usize k = 0; k < n; ++k)
            {
                const f64 nr = pr * poles[i].re - pim * poles[i].im;
                pim = pr * poles[i].im + pim * poles[i].re;
                pr = nr;
            }
            re += res[i].re * pr - res[i].im * pim;
        }
        INFO("residuez n=" << n);
        CHECK_THAT(re, WithinAbs(h[n], 1e-9));
    }
    // lifter: low-time keeps q<3, zeros the rest.
    cont::Array<f64> c(&alloc);
    c.resize(8);
    for (usize i = 0; i < 8; ++i)
    {
        c[i] = 1.0;
    }
    dsp::lifter<f64>(cont::Span<f64>(c.data(), 8), 3, true);
    CHECK(c[2] == 1.0);
    CHECK(c[3] == 0.0);
}

TEST_CASE("dsp transforms: fwht self-inverse + known case", "[v11-q][dsp][transforms]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    // known: fwht([1,0,1,0,1,0,1,0]) — only the DC and one sequency component survive.
    cont::Array<f64> x(&alloc);
    x.resize(8);
    for (usize i = 0; i < 8; ++i)
    {
        x[i] = (i % 2 == 0) ? 1.0 : 0.0;
    }
    cont::Array<f64> y(&alloc);
    y.resize(8);
    for (usize i = 0; i < 8; ++i)
    {
        y[i] = x[i];
    }
    dsp::fwht<f64>(cont::Span<f64>(y.data(), 8));
    CHECK_THAT(y[0], WithinAbs(4.0, 1e-12)); // sum = 4
    // self-inverse: fwht(fwht(x)) == 8·x.
    dsp::fwht<f64>(cont::Span<f64>(y.data(), 8));
    for (usize i = 0; i < 8; ++i)
    {
        INFO("i=" << i);
        CHECK_THAT(y[i], WithinAbs(8.0 * x[i], 1e-12));
    }
}
