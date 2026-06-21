// crd-hesap-wavelet v11w-d — continuous wavelet transform.
// Gates: coefficients vs pywt.cwt (mexh/morl/gaus real; cmor/cgau/shan/fbsp complex) + central frequencies vs pywt;
// paul self-contained (tone localizes to fc/f); run-twice + {1,4,16}-thread bit-identical (the batched-FFT moat).

#include <crd/hesap/wavelet/cwt.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstring>
#include <numbers>

namespace wv = crd::hesap::wavelet;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using crd::hesap::Complex;
using Catch::Matchers::WithinAbs;

namespace
{
#include "cwt_refs.inc"
constexpr f64 kPi = std::numbers::pi_v<f64>;

usize cwt_n() { return sizeof(ref_cwt_input) / sizeof(ref_cwt_input[0]); }
usize cwt_ns() { return sizeof(ref_cwt_scales) / sizeof(ref_cwt_scales[0]); }
} // namespace

TEST_CASE("cwt: real wavelets vs pywt (mexh/morl/gaus)", "[v11w-d][wavelet][cwt]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize n = cwt_n();
    const usize ns = cwt_ns();
    const cont::ConstSpan<f64> xs(ref_cwt_input, n);
    const cont::ConstSpan<f64> sc(ref_cwt_scales, ns);

    struct C
    {
        const char* name;
        const double* ref;
        const double* freqs;
    };
    for (const auto& tc : {C{"mexh", ref_cwt_mexh, ref_cwt_mexh_freqs}, C{"morl", ref_cwt_morl, ref_cwt_morl_freqs},
                           C{"gaus2", ref_cwt_gaus2, ref_cwt_gaus2_freqs}, C{"gaus4", ref_cwt_gaus4, ref_cwt_gaus4_freqs}})
    {
        const auto w = wv::continuous_wavelet(tc.name);
        const auto r = wv::cwt<f64>(&alloc, xs, sc, w);
        INFO(tc.name);
        for (usize s = 0; s < ns; ++s)
        {
            CHECK_THAT(r.frequencies[s], WithinAbs(tc.freqs[s], 1e-9));
            for (usize i = 0; i < n; ++i)
            {
                INFO("scale " << s << " idx " << i);
                CHECK_THAT(r.coeffs[s * n + i].re, WithinAbs(tc.ref[s * n + i], 1e-8));
                CHECK_THAT(r.coeffs[s * n + i].im, WithinAbs(0.0, 1e-9));
            }
        }
    }
}

TEST_CASE("cwt: complex wavelets vs pywt (cmor/cgau/shan/fbsp)", "[v11w-d][wavelet][cwt]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize n = cwt_n();
    const usize ns = cwt_ns();
    const cont::ConstSpan<f64> xs(ref_cwt_input, n);
    const cont::ConstSpan<f64> sc(ref_cwt_scales, ns);

    struct C
    {
        const char* name;
        const double* ref;
        const double* freqs;
    };
    for (const auto& tc : {C{"cmor1.5-1.0", ref_cwt_cmor1_5_1_0, ref_cwt_cmor1_5_1_0_freqs},
                           C{"cgau2", ref_cwt_cgau2, ref_cwt_cgau2_freqs},
                           C{"shan1.0-0.5", ref_cwt_shan1_0_0_5, ref_cwt_shan1_0_0_5_freqs},
                           C{"fbsp2-1.0-0.5", ref_cwt_fbsp2_1_0_0_5, ref_cwt_fbsp2_1_0_0_5_freqs}})
    {
        const auto w = wv::continuous_wavelet(tc.name);
        REQUIRE(w.complex_valued);
        const auto r = wv::cwt<f64>(&alloc, xs, sc, w);
        INFO(tc.name);
        for (usize s = 0; s < ns; ++s)
        {
            CHECK_THAT(r.frequencies[s], WithinAbs(tc.freqs[s], 1e-7));
            for (usize i = 0; i < n; ++i)
            {
                INFO("scale " << s << " idx " << i);
                CHECK_THAT(r.coeffs[s * n + i].re, WithinAbs(tc.ref[2 * (s * n + i)], 1e-8));
                CHECK_THAT(r.coeffs[s * n + i].im, WithinAbs(tc.ref[2 * (s * n + i) + 1], 1e-8));
            }
        }
    }
}

TEST_CASE("cwt: a tone localizes to scale ~ fc/f (morl + paul)", "[v11w-d][wavelet][cwt]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    const usize n = 512;
    const f64 f0 = 0.05;
    cont::Array<f64> x(&alloc);
    x.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        x[i] = std::cos(2.0 * kPi * f0 * static_cast<f64>(i));
    }
    cont::Array<f64> scales(&alloc);
    const usize ns = 48;
    scales.resize(ns);
    for (usize s = 0; s < ns; ++s)
    {
        scales[s] = std::pow(64.0, static_cast<f64>(s) / static_cast<f64>(ns - 1));
    }
    for (const char* name : {"morl", "paul4"}) // paul is not in pywt — self-contained physics gate
    {
        const auto w = wv::continuous_wavelet(name);
        const f64 fc = wv::central_frequency(&alloc, w);
        const auto r =
            wv::cwt<f64>(&alloc, cont::ConstSpan<f64>(x.data(), n), cont::ConstSpan<f64>(scales.data(), ns), w);
        usize peak = 0;
        f64 bestp = -1.0;
        for (usize s = 0; s < ns; ++s)
        {
            f64 p = 0.0;
            for (usize i = n / 4; i < 3 * n / 4; ++i)
            {
                const Complex<f64> c = r.coeffs[s * n + i];
                p += c.re * c.re + c.im * c.im;
            }
            if (p > bestp)
            {
                bestp = p;
                peak = s;
            }
        }
        const f64 expected = fc / f0;
        INFO(name << " peak scale " << scales[peak] << " expected ~" << expected);
        CHECK(scales[peak] > expected * 0.7);
        CHECK(scales[peak] < expected * 1.43);
    }
}

TEST_CASE("cwt: run-twice + {1,4,16}-thread bit-identical (batched-FFT moat)", "[v11w-d][wavelet][cwt][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 25);
    const usize n = cwt_n();
    const usize ns = cwt_ns();
    const cont::ConstSpan<f64> xs(ref_cwt_input, n);
    const cont::ConstSpan<f64> sc(ref_cwt_scales, ns);
    const auto w = wv::continuous_wavelet("cmor1.5-1.0");

    cont::Array<Complex<f64>> ref(&alloc);
    bool have = false;
    for (crd::u32 nw : {1U, 4U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            const auto r = wv::cwt<f64>(&alloc, xs, sc, w);
            if (!have)
            {
                ref.resize(r.coeffs.size());
                for (usize i = 0; i < r.coeffs.size(); ++i)
                {
                    ref[i] = r.coeffs[i];
                }
                have = true;
            }
            else
            {
                INFO("threads " << nw);
                CHECK(std::memcmp(r.coeffs.data(), ref.data(), ref.size() * sizeof(Complex<f64>)) == 0);
            }
        }
        crd::jobs::shutdown();
    }
}
