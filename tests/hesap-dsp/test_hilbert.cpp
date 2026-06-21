// crd-hesap-dsp v11-l — Hilbert / analytic signal. Gated on real+imag vs scipy.signal.hilbert (~1e-10), the
// analytic property (Re(x_a) == x), envelope vs scipy, and a linear chirp's instantaneous frequency vs scipy.

#include <crd/hesap/dsp/hilbert.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "hilbert2_refs.inc"

#include "hilbert_refs.inc"

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
    const f64 fn = static_cast<f64>(n);
    for (usize i = 0; i < n; ++i)
    {
        const f64 fi = static_cast<f64>(i);
        x[i] = std::sin(2 * kPi * 5 * fi / fn) + 0.5 * std::cos(2 * kPi * 12 * fi / fn);
    }
    return x;
}
} // namespace

TEST_CASE("dsp hilbert: analytic signal matches scipy (even + odd length)", "[v11-l][dsp][hilbert]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    for (usize n : {usize{64}, usize{63}})
    {
        const auto x = sig(&alloc, n);
        const auto a = dsp::hilbert<f64>(&alloc, cont::ConstSpan<f64>(x.data(), n));
        REQUIRE(a.size() == n);
        const double* ref_re = (n == 64) ? ref_hilbert_x64_re : ref_hilbert_x63_re;
        const double* ref_im = (n == 64) ? ref_hilbert_x64_im : ref_hilbert_x63_im;
        for (usize i = 0; i < n; ++i)
        {
            INFO("n=" << n << " i=" << i);
            CHECK_THAT(a[i].re, WithinAbs(ref_re[i], 1e-10));
            CHECK_THAT(a[i].im, WithinAbs(ref_im[i], 1e-10));
            CHECK_THAT(a[i].re, WithinAbs(x[i], 1e-10)); // analytic property: Re(x_a) == x
        }
    }
}

TEST_CASE("dsp hilbert: envelope of an AM signal matches scipy", "[v11-l][dsp][hilbert]")
{
    crd::memory::TlsfAllocator alloc(1U << 21);
    const usize n = 256;
    cont::Array<f64> am(&alloc);
    am.resize(n);
    const f64 fn = static_cast<f64>(n);
    for (usize i = 0; i < n; ++i)
    {
        const f64 fi = static_cast<f64>(i);
        am[i] = (1 + 0.5 * std::cos(2 * kPi * 3 * fi / fn)) * std::cos(2 * kPi * 40 * fi / fn);
    }
    const auto e = dsp::envelope<f64>(&alloc, cont::ConstSpan<f64>(am.data(), n));
    REQUIRE(e.size() == n);
    for (usize i = 0; i < n; ++i)
    {
        INFO("i=" << i);
        CHECK_THAT(e[i], WithinAbs(ref_envelope_am[i], 1e-10));
    }
}

TEST_CASE("dsp hilbert: hilbert2 matches scipy.signal.hilbert2", "[v11-l][dsp][hilbert]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize r = 8;
    const usize c = 8;
    cont::Array<f64> img(&alloc);
    img.resize(r * c);
    for (usize i = 0; i < r; ++i)
    {
        for (usize j = 0; j < c; ++j)
        {
            img[i * c + j] = std::cos(0.4 * static_cast<f64>(i) + 0.2 * static_cast<f64>(j)) +
                             0.3 * std::sin(0.7 * static_cast<f64>(j));
        }
    }
    const auto a = dsp::hilbert2<f64>(&alloc, cont::ConstSpan<f64>(img.data(), r * c), r, c);
    REQUIRE(a.size() == r * c);
    for (usize i = 0; i < r * c; ++i) // vs scipy hilbert2 (the outer-product 2D step; Re is NOT the input)
    {
        INFO("i=" << i);
        CHECK_THAT(a[i].re, WithinAbs(ref_h2_re[i], 1e-9));
        CHECK_THAT(a[i].im, WithinAbs(ref_h2_im[i], 1e-9));
    }
}

TEST_CASE("dsp hilbert: instantaneous frequency of a linear chirp matches scipy", "[v11-l][dsp][hilbert]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize n = 512;
    const f64 fs = 512.0;
    const f64 f0 = 20.0;
    const f64 f1 = 100.0;
    const f64 tt = n / fs;
    cont::Array<f64> ch(&alloc);
    ch.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        const f64 t = static_cast<f64>(i) / fs;
        ch[i] = std::cos(2 * kPi * (f0 * t + 0.5 * (f1 - f0) / tt * t * t));
    }
    const auto inst = dsp::instantaneous_frequency<f64>(&alloc, cont::ConstSpan<f64>(ch.data(), n), fs);
    REQUIRE(inst.size() == n - 1);
    // interior bins (edges have Hilbert artifacts in both scipy and ours — exact match there too, gate the interior).
    for (usize i = 50; i < 450; ++i)
    {
        INFO("i=" << i);
        CHECK_THAT(inst[i], WithinAbs(ref_instfreq_chirp[i], 1e-9));
        // physical check: recovered freq tracks the linear sweep f0 + (f1-f0)/T * t.
        const f64 expected = f0 + (f1 - f0) / tt * (static_cast<f64>(i) / fs);
        CHECK_THAT(inst[i], WithinAbs(expected, 1.5));
    }
}
