// crd-hesap-dsp v11-k — multirate. Gated vs scipy upfirdn / resample_poly / decimate (~1e-9) + the run-twice
// determinism moat (single-thread, fixed tap order ⇒ bit-identical across runs).

#include <crd/hesap/dsp/multirate.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "multirate_refs.inc"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstring>
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
        x[i] = std::sin(2 * kPi * 0.05 * fi) + 0.5 * std::sin(2 * kPi * 0.13 * fi);
    }
    return x;
}

template <usize N> void check(const double (&ref)[N], const cont::Array<f64>& got, double tol)
{
    REQUIRE(got.size() == N);
    for (usize i = 0; i < N; ++i)
    {
        INFO("i=" << i);
        CHECK_THAT(got[i], WithinAbs(ref[i], tol));
    }
}
} // namespace

TEST_CASE("dsp multirate: upfirdn matches scipy", "[v11-k][dsp][multirate]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const f64 h[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    cont::Array<f64> x(&alloc);
    x.resize(20);
    for (usize i = 0; i < 20; ++i)
    {
        x[i] = static_cast<f64>(i + 1);
    }
    const auto y = dsp::upfirdn<f64>(&alloc, cont::ConstSpan<f64>(h, 5), cont::ConstSpan<f64>(x.data(), 20), 3, 2);
    check(ref_upfirdn_3_2, y, 1e-12);
}

TEST_CASE("dsp multirate: resample_poly matches scipy (up + down)", "[v11-k][dsp][multirate]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto x = sig(&alloc, 500);
    const cont::ConstSpan<f64> xs(x.data(), 500);
    check(ref_resample_3_2, dsp::resample_poly<f64>(&alloc, xs, 3, 2), 1e-9);
    check(ref_resample_2_3, dsp::resample_poly<f64>(&alloc, xs, 2, 3), 1e-9);
}

TEST_CASE("dsp multirate: decimate (FIR) matches scipy", "[v11-k][dsp][multirate]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto x = sig(&alloc, 600);
    check(ref_decimate_q4, dsp::decimate<f64>(&alloc, cont::ConstSpan<f64>(x.data(), 600), 4), 1e-9);
}

TEST_CASE("dsp multirate: resample (FFT) band-limited interpolation is exact for a periodic tone", "[v11-k][dsp][multirate]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize n = 100, num = 200;
    cont::Array<f64> x(&alloc);
    x.resize(n);
    for (usize i = 0; i < n; ++i) // exactly 5 cycles over N ⇒ periodic, no leakage
    {
        x[i] = std::cos(2 * kPi * 5.0 * static_cast<f64>(i) / static_cast<f64>(n));
    }
    const auto y = dsp::resample<f64>(&alloc, cont::ConstSpan<f64>(x.data(), n), num);
    REQUIRE(y.size() == num);
    for (usize m = 0; m < num; ++m) // resampled ⇒ 5 cycles over num
    {
        INFO("m=" << m);
        CHECK_THAT(y[m], WithinAbs(std::cos(2 * kPi * 5.0 * static_cast<f64>(m) / static_cast<f64>(num)), 1e-9));
    }
}

TEST_CASE("dsp multirate: interp/half_band/cic/farrow", "[v11-k][dsp][multirate]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // interp by q ⇒ q×N output.
    cont::Array<f64> s(&alloc);
    s.resize(50);
    for (usize i = 0; i < 50; ++i)
    {
        s[i] = std::sin(0.2 * static_cast<f64>(i));
    }
    CHECK(dsp::interp<f64>(&alloc, cont::ConstSpan<f64>(s.data(), 50), 3).size() == 150);
    // half_band: centre tap 0.5, every even offset from centre is zero, symmetric.
    const auto hb = dsp::half_band<f64>(&alloc, 11);
    CHECK_THAT(hb[5], WithinAbs(0.5, 1e-12));
    for (usize i : {1u, 3u, 7u, 9u})
    {
        CHECK_THAT(hb[i], WithinAbs(0.0, 1e-12)); // even offsets from centre vanish
    }
    CHECK_THAT(hb[4], WithinAbs(hb[6], 1e-12)); // symmetric
    // CIC: constant input ⇒ steady-state output = (R·M)^N · const.
    cont::Array<f64> c(&alloc);
    c.resize(200);
    for (usize i = 0; i < 200; ++i)
    {
        c[i] = 1.0;
    }
    const auto cy = dsp::cic_decimate<f64>(&alloc, cont::ConstSpan<f64>(c.data(), 200), 4, 2, 1);
    CHECK_THAT(cy[cy.size() - 1], WithinAbs(16.0, 1e-9)); // (4·1)^2 = 16
    // Farrow cubic: a linear ramp delayed by mu is exactly interpolated.
    cont::Array<f64> ramp(&alloc);
    ramp.resize(40);
    for (usize i = 0; i < 40; ++i)
    {
        ramp[i] = static_cast<f64>(i);
    }
    const auto fd = dsp::farrow_delay<f64>(&alloc, cont::ConstSpan<f64>(ramp.data(), 40), 0.3);
    for (usize i = 5; i < 35; ++i)
    {
        INFO("i=" << i);
        CHECK_THAT(fd[i], WithinAbs(static_cast<f64>(i) - 0.3, 1e-9));
    }
}

TEST_CASE("dsp multirate: resample_poly is deterministic (run-twice bit-identical)", "[v11-k][dsp][multirate]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto x = sig(&alloc, 1000);
    const cont::ConstSpan<f64> xs(x.data(), 1000);
    const auto a = dsp::resample_poly<f64>(&alloc, xs, 5, 3);
    const auto b = dsp::resample_poly<f64>(&alloc, xs, 5, 3);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(f64)) == 0); // the determinism moat
}
