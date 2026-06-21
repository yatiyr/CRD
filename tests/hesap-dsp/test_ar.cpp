// crd-hesap-dsp v11-o — parametric AR. Self-contained gate: recover a known AR(2) process's coefficients via
// Yule-Walker + Burg (a deterministic white sequence driven through a known pole pair) + reflection |k|<1 +
// AIC/MDL order selection picking the true order + the AR-PSD peaking at the resonance.

#include <crd/hesap/dsp/ar.hpp>
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

// AR(2) with poles r·e^{±jθ}: x[n] = a1·x[n-1] + a2·x[n-2] + e[n]; a1 = 2r cosθ, a2 = -r².
cont::Array<f64> ar2_signal(crd::memory::IAllocator* a, usize n, f64 r, f64 theta)
{
    const f64 a1 = 2.0 * r * std::cos(theta), a2 = -r * r;
    cont::Array<f64> x(a);
    x.resize(n);
    u64 s = 99991ULL;
    f64 xm1 = 0.0, xm2 = 0.0;
    for (usize i = 0; i < n; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const f64 e = (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
        const f64 xn = a1 * xm1 + a2 * xm2 + e;
        x[i] = xn;
        xm2 = xm1;
        xm1 = xn;
    }
    return x;
}
} // namespace

TEST_CASE("dsp ar: Yule-Walker + Burg recover a known AR(2) process", "[v11-o][dsp][ar]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const f64 r = 0.9, theta = kPi / 4.0;
    const f64 a1 = 2.0 * r * std::cos(theta), a2 = -r * r;
    const auto x = ar2_signal(&alloc, 8000, r, theta);
    const cont::ConstSpan<f64> xs(x.data(), x.size());
    for (const char* which : {"yule", "burg"})
    {
        const auto m = (which[0] == 'y') ? dsp::aryule<f64>(&alloc, xs, 2) : dsp::arburg<f64>(&alloc, xs, 2);
        INFO(which);
        REQUIRE(m.a.size() == 3);
        CHECK_THAT(m.a[1], WithinAbs(-a1, 0.05)); // A(z) = 1 - a1 z⁻¹ - a2 z⁻²
        CHECK_THAT(m.a[2], WithinAbs(-a2, 0.05));
        CHECK(std::abs(m.reflection[0]) < 1.0); // stability
        CHECK(std::abs(m.reflection[1]) < 1.0);
        CHECK(m.variance > 0.0);
    }
}

TEST_CASE("dsp ar: covariance + modified-covariance recover a known AR(2)", "[v11-o][dsp][ar]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const f64 r = 0.9, theta = kPi / 4.0;
    const f64 a1 = 2.0 * r * std::cos(theta), a2 = -r * r;
    const auto x = ar2_signal(&alloc, 4000, r, theta);
    const cont::ConstSpan<f64> xs(x.data(), x.size());
    for (const char* which : {"cov", "mcov"})
    {
        const auto m = (which[1] == 'o') ? dsp::arcov<f64>(&alloc, xs, 2) : dsp::armcov<f64>(&alloc, xs, 2);
        INFO(which);
        CHECK_THAT(m.a[1], WithinAbs(-a1, 0.05));
        CHECK_THAT(m.a[2], WithinAbs(-a2, 0.05));
        CHECK(m.variance > 0.0);
    }
}

TEST_CASE("dsp ar: AR-PSD peaks at the resonance frequency", "[v11-o][dsp][ar]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const f64 r = 0.95, theta = kPi / 3.0; // resonance at θ/(2π) cycles/sample
    const auto x = ar2_signal(&alloc, 8000, r, theta);
    const auto m = dsp::arburg<f64>(&alloc, cont::ConstSpan<f64>(x.data(), x.size()), 2);
    const usize nfft = 1024;
    const auto psd = dsp::ar_psd<f64>(&alloc, m, nfft);
    usize peak = 0;
    for (usize i = 1; i < nfft / 2; ++i)
    {
        if (psd[i] > psd[peak])
        {
            peak = i;
        }
    }
    const f64 peak_freq = static_cast<f64>(peak) / static_cast<f64>(nfft); // cycles/sample
    CHECK_THAT(peak_freq, WithinAbs(theta / (2.0 * kPi), 0.01));
}

TEST_CASE("dsp ar: AIC/MDL select the true order", "[v11-o][dsp][ar]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto x = ar2_signal(&alloc, 8000, 0.9, kPi / 4.0);
    const cont::ConstSpan<f64> xs(x.data(), x.size());
    f64 best_aic = 1e300;
    usize best_p = 0;
    for (usize p = 1; p <= 8; ++p)
    {
        const auto m = dsp::arburg<f64>(&alloc, xs, p);
        const f64 aic = dsp::ar_aic<f64>(x.size(), m.variance, p);
        if (aic < best_aic)
        {
            best_aic = aic;
            best_p = p;
        }
    }
    CHECK(best_p == 2); // the true AR order
}
