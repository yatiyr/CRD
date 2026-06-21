// crd-hesap-dsp v11-d (type III/IV remez) — antisymmetric equiripple FIR: Hilbert transformer + differentiator.
// Spec-compliance gate: the amplitude response A(w) = 2·Σ h[M-n]·sin(nw) matches the target in band (the design is
// iterative-transcendental ⇒ spec-compliance, not bit-match).

#include <crd/hesap/dsp/remez.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

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

// amplitude response of an antisymmetric type-III filter: A(w) = 2·Σ_{n=1}^{M} h[M-n]·sin(nw).
f64 amp(const cont::Array<f64>& h, f64 w)
{
    const usize m = (h.size() - 1) / 2;
    f64 a = 0.0;
    for (usize n = 1; n <= m; ++n)
    {
        a += h[m - n] * std::sin(static_cast<f64>(n) * w);
    }
    return 2.0 * a;
}
} // namespace

TEST_CASE("dsp remez: Hilbert transformer (type III) is ~unity in band + antisymmetric", "[v11-d][dsp][remez]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize numtaps = 31;
    const auto h = dsp::remez_hilbert<f64>(&alloc, numtaps, 0.05, 0.45);
    REQUIRE(h.size() == numtaps);
    const usize m = (numtaps - 1) / 2;
    CHECK_THAT(h[m], WithinAbs(0.0, 1e-12)); // centre tap is 0
    for (usize n = 1; n <= m; ++n)
    {
        CHECK_THAT(h[m - n], WithinAbs(-h[m + n], 1e-12)); // antisymmetric
    }
    for (f64 f = 0.08; f <= 0.42; f += 0.01) // |A(w)| ≈ 1 across the passband (equiripple within a few %)
    {
        INFO("f=" << f);
        CHECK_THAT(std::abs(amp(h, 2 * kPi * f)), WithinAbs(1.0, 0.03));
    }
}

TEST_CASE("dsp remez: differentiator (type III) has |H(w)| ~ w", "[v11-d][dsp][remez]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const usize numtaps = 41;
    const auto h = dsp::remez_differentiator<f64>(&alloc, numtaps, 0.4);
    REQUIRE(h.size() == numtaps);
    for (f64 f = 0.05; f <= 0.35; f += 0.02) // A(w) ≈ 2π·f (the ideal differentiator gain)
    {
        const f64 w = 2 * kPi * f;
        INFO("f=" << f);
        CHECK_THAT(amp(h, w), WithinAbs(w, 0.05 * w + 0.02));
    }
}
