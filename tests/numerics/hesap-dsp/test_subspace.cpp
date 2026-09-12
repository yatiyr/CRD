// crd-hesap-dsp v11-p — subspace (super-resolution). The killer gate: root-MUSIC + MUSIC resolve two tones
// separated by LESS than the FFT bin width 1/N — the super-resolution a periodogram cannot achieve. Self-contained
// (recovery of planted frequencies IS the spec).

#include <crd/hesap/dsp/subspace.hpp>
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
using Catch::Matchers::WithinAbs;

namespace
{
constexpr f64 kPi = std::numbers::pi_v<f64>;

// two real tones at f1,f2 + a little noise. N=128 ⇒ FFT bin = 1/128 ≈ 0.0078.
cont::Array<f64> two_tones(crd::memory::IAllocator* a, usize n, f64 f1, f64 f2, f64 noise_amp)
{
    cont::Array<f64> x(a);
    x.resize(n);
    u64 s = 555ULL;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const f64 e = (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
        const f64 t = static_cast<f64>(i);
        x[i] = std::cos(2 * kPi * f1 * t + 0.3) + std::cos(2 * kPi * f2 * t + 1.1) + noise_amp * e;
    }
    return x;
}
} // namespace

TEST_CASE("dsp subspace: root-MUSIC resolves tones below the FFT bin width", "[v11-p][dsp][subspace]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize n = 128;
    const usize m = 40;
    const f64 f1 = 0.200;
    const f64 f2 = 0.206; // Δf = 0.006 < FFT bin 1/128 ≈ 0.0078 ⇒ a periodogram CANNOT resolve
    const auto x = two_tones(&alloc, n, f1, f2, 0.02);
    const auto f = dsp::root_music<f64>(&alloc, cont::ConstSpan<f64>(x.data(), n), m, 2);
    REQUIRE(f.size() == 2);
    INFO("recovered " << f[0] << ", " << f[1]);
    CHECK_THAT(f[0], WithinAbs(f1, 0.004));
    CHECK_THAT(f[1], WithinAbs(f2, 0.004));
}

TEST_CASE("dsp subspace: ESPRIT + min-norm resolve sub-FFT-bin tones", "[v11-p][dsp][subspace]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize n = 128;
    const usize m = 40;
    const f64 f1 = 0.200;
    const f64 f2 = 0.206;
    const auto x = two_tones(&alloc, n, f1, f2, 0.02);
    const cont::ConstSpan<f64> xs(x.data(), n);
    const auto fe = dsp::esprit<f64>(&alloc, xs, m, 2);
    REQUIRE(fe.size() == 2);
    INFO("esprit " << fe[0] << ", " << fe[1]);
    CHECK_THAT(fe[0], WithinAbs(f1, 0.004));
    CHECK_THAT(fe[1], WithinAbs(f2, 0.004));
    const auto fm = dsp::min_norm<f64>(&alloc, xs, m, 2);
    REQUIRE(fm.size() == 2);
    INFO("min_norm " << fm[0] << ", " << fm[1]);
    CHECK_THAT(fm[0], WithinAbs(f1, 0.004));
    CHECK_THAT(fm[1], WithinAbs(f2, 0.004));
}

TEST_CASE("dsp subspace: MUSIC pseudospectrum peaks at the tones", "[v11-p][dsp][subspace]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize n = 128;
    const usize m = 40;
    const usize nfreq = 2000;
    const f64 f1 = 0.20;
    const f64 f2 = 0.25;
    const auto x = two_tones(&alloc, n, f1, f2, 0.02);
    const auto p = dsp::music_spectrum<f64>(&alloc, cont::ConstSpan<f64>(x.data(), n), m, 2, nfreq);
    // the two highest local maxima should sit at f1 and f2 (frequency grid covers [0, 0.5)).
    auto bin_to_f = [&](usize b) { return 0.5 * static_cast<f64>(b) / static_cast<f64>(nfreq); };
    usize p1 = 0;
    for (usize i = 1; i < nfreq; ++i)
    {
        if (p[i] > p[p1])
        {
            p1 = i;
        }
    }
    usize p2 = 0;
    for (usize i = 1; i < nfreq; ++i)
    {
        if (p[i] > p[p2] && std::abs(bin_to_f(i) - bin_to_f(p1)) > 0.02)
        {
            p2 = i;
        }
    }
    f64 lo = bin_to_f(p1 < p2 ? p1 : p2);
    f64 hi = bin_to_f(p1 < p2 ? p2 : p1);
    CHECK_THAT(lo, WithinAbs(f1, 0.003));
    CHECK_THAT(hi, WithinAbs(f2, 0.003));
}
