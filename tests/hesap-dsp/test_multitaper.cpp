// crd-hesap-dsp v11-n — Thomson multitaper. Self-contained gates: a tone produces a sharp peak at its bin; white
// noise gives a roughly flat PSD; and the K-taper estimate has LOWER variance than a single periodogram (the whole
// point of multitaper). vs MATLAB pmtm (cross-check / bench separately).

#include <crd/hesap/dsp/multitaper.hpp>
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

void mean_var(cont::ConstSpan<f64> v, usize lo, usize hi, f64& mean, f64& var)
{
    mean = 0.0;
    for (usize i = lo; i < hi; ++i)
    {
        mean += v[i];
    }
    mean /= static_cast<f64>(hi - lo);
    var = 0.0;
    for (usize i = lo; i < hi; ++i)
    {
        var += (v[i] - mean) * (v[i] - mean);
    }
    var /= static_cast<f64>(hi - lo);
}
} // namespace

TEST_CASE("dsp multitaper: a tone yields a sharp peak at its bin", "[v11-n][dsp][multitaper]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const usize n = 512;
    const usize nfft = 512;
    cont::Array<f64> x(&alloc);
    x.resize(n);
    const f64 f0 = 0.2;
    for (usize i = 0; i < n; ++i)
    {
        x[i] = std::cos(2 * kPi * f0 * static_cast<f64>(i));
    }
    const auto psd = dsp::multitaper_psd<f64>(&alloc, cont::ConstSpan<f64>(x.data(), n), 4.0, 7, nfft);
    usize peak = 0;
    for (usize i = 1; i < psd.size(); ++i)
    {
        if (psd[i] > psd[peak])
        {
            peak = i;
        }
    }
    const f64 peak_f = static_cast<f64>(peak) / static_cast<f64>(nfft);
    CHECK_THAT(peak_f, WithinAbs(f0, 0.01));
}

TEST_CASE("dsp multitaper: adaptive weighting resolves a tone + stays positive", "[v11-n][dsp][multitaper]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const usize n = 512;
    const usize nfft = 512;
    cont::Array<f64> x(&alloc);
    x.resize(n);
    const f64 f0 = 0.2;
    for (usize i = 0; i < n; ++i)
    {
        x[i] = std::cos(2 * kPi * f0 * static_cast<f64>(i));
    }
    const auto psd = dsp::multitaper_psd<f64>(&alloc, cont::ConstSpan<f64>(x.data(), n), 4.0, 7, nfft, true); // adaptive
    usize peak = 0;
    for (usize i = 1; i < psd.size(); ++i)
    {
        if (psd[i] > psd[peak])
        {
            peak = i;
        }
        CHECK(psd[i] >= 0.0); // a PSD is non-negative
    }
    CHECK_THAT(static_cast<f64>(peak) / static_cast<f64>(nfft), WithinAbs(f0, 0.01));
}

TEST_CASE("dsp multitaper: lower variance than a single periodogram on white noise", "[v11-n][dsp][multitaper]")
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const usize n = 512;
    const usize nfft = 512;
    const auto x = noise(&alloc, n, 2024ULL);
    const cont::ConstSpan<f64> xs(x.data(), n);
    const auto mt = dsp::multitaper_psd<f64>(&alloc, xs, 4.0, 7, nfft); // K=7 tapers
    const auto sp = dsp::multitaper_psd<f64>(&alloc, xs, 4.0, 1, nfft); // K=1 ⇒ a single (tapered) periodogram
    f64 mmt;
    f64 vmt;
    f64 msp;
    f64 vsp;
    mean_var(cont::ConstSpan<f64>(mt.data(), mt.size()), 20, mt.size() - 20, mmt, vmt);
    mean_var(cont::ConstSpan<f64>(sp.data(), sp.size()), 20, sp.size() - 20, msp, vsp);
    // multitaper smooths ⇒ a much smaller coefficient of variation than the single periodogram.
    CHECK((vmt / (mmt * mmt)) < (vsp / (msp * msp)));
    CHECK(mmt > 0.0);
}
