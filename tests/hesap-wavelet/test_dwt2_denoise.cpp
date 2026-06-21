// crd-hesap-wavelet v11w-e — 2-D DWT + denoising.
// Gates: dwt2 subbands vs pywt + idwt2/waverec2 round trip; threshold (soft/hard/garrote) vs pywt; denoise
// improves SNR on a noisy signal (self-contained); run-twice bit-identical.

#include <crd/hesap/wavelet/denoise.hpp>
#include <crd/hesap/wavelet/dwt2.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstring>

namespace wv = crd::hesap::wavelet;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using crd::u64;
using Catch::Matchers::WithinAbs;
using Mode = wv::SignalExtensionMode;

namespace
{
#include "dwt2_refs.inc"

template <usize N> void check_arr(const double (&ref)[N], const cont::Array<f64>& got, f64 tol)
{
    REQUIRE(got.size() == N);
    for (usize i = 0; i < N; ++i)
    {
        INFO("idx " << i);
        CHECK_THAT(got[i], WithinAbs(ref[i], tol));
    }
}
} // namespace

TEST_CASE("dwt2: subbands vs pywt + round trip", "[v11w-e][wavelet][dwt2]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize R = static_cast<usize>(ref_img_rows);
    const usize C = static_cast<usize>(ref_img_cols);

// NOLINTBEGIN(cppcoreguidelines-macro-usage,bugprone-macro-parentheses,readability-isolate-declaration) -- token-paste helper
#define CHECK_DWT2(WAV, MODESAN, MODEENUM)                                                                              \
    do                                                                                                                  \
    {                                                                                                                   \
        const auto w = wv::wavelet_by_name(WAV);                                                                        \
        const auto d = wv::dwt2<f64>(&alloc, ref_img, R, C, *w, Mode::MODEENUM);                                        \
        INFO(WAV << " " << #MODESAN);                                                                                   \
        check_arr(ref_dwt2_##MODESAN##_cA, d.cA, 1e-10);                                                                \
        check_arr(ref_dwt2_##MODESAN##_cH, d.cH, 1e-10);                                                                \
        check_arr(ref_dwt2_##MODESAN##_cV, d.cV, 1e-10);                                                                \
        check_arr(ref_dwt2_##MODESAN##_cD, d.cD, 1e-10);                                                                \
        cont::Array<f64> rec(&alloc);                                                                                   \
        usize rr = 0, rc = 0;                                                                                           \
        wv::idwt2<f64>(&alloc, d, *w, Mode::MODEENUM, rec, rr, rc);                                                     \
        REQUIRE(rr >= R);                                                                                               \
        REQUIRE(rc >= C);                                                                                               \
        for (usize i = 0; i < R; ++i)                                                                                   \
            for (usize j = 0; j < C; ++j)                                                                               \
            {                                                                                                          \
                INFO("recon " << i << "," << j);                                                                        \
                CHECK_THAT(rec[i * rc + j], WithinAbs(ref_img[i * C + j], 1e-9));                                       \
            }                                                                                                          \
    } while (0)

    CHECK_DWT2("haar", haar_periodization, Periodization);
    CHECK_DWT2("db2", db2_periodization, Periodization);
    CHECK_DWT2("haar", haar_symmetric, Symmetric);
    CHECK_DWT2("db2", db2_symmetric, Symmetric);
#undef CHECK_DWT2
    // NOLINTEND(cppcoreguidelines-macro-usage,bugprone-macro-parentheses,readability-isolate-declaration)
}

TEST_CASE("wavedec2: multilevel 2-D round trip", "[v11w-e][wavelet][dwt2]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize r = static_cast<usize>(ref_img_rows);
    const usize c = static_cast<usize>(ref_img_cols);
    const auto w = wv::wavelet_by_name("db2");
    const auto dec = wv::wavedec2<f64>(&alloc, ref_img, r, c, *w, Mode::Periodization, 2);
    REQUIRE(dec.details.size() == 2);
    cont::Array<f64> rec(&alloc);
    usize rr = 0;
    usize rc = 0;
    rec = wv::waverec2<f64>(&alloc, dec, *w, Mode::Periodization, rr, rc);
    REQUIRE(rr == r);
    REQUIRE(rc == c);
    for (usize i = 0; i < r * c; ++i)
    {
        INFO("idx " << i);
        CHECK_THAT(rec[i], WithinAbs(ref_img[i], 1e-9));
    }
}

TEST_CASE("threshold: soft/hard/garrote vs pywt", "[v11w-e][wavelet][denoise]")
{
    const usize n = sizeof(ref_thr_in) / sizeof(ref_thr_in[0]);
    for (usize i = 0; i < n; ++i)
    {
        INFO("idx " << i);
        CHECK_THAT(wv::threshold_value<f64>(ref_thr_in[i], 1.0, wv::ThresholdMode::Soft), WithinAbs(ref_thr_soft[i], 1e-12));
        CHECK_THAT(wv::threshold_value<f64>(ref_thr_in[i], 1.0, wv::ThresholdMode::Hard), WithinAbs(ref_thr_hard[i], 1e-12));
        CHECK_THAT(wv::threshold_value<f64>(ref_thr_in[i], 1.0, wv::ThresholdMode::Garrote),
                   WithinAbs(ref_thr_garrote[i], 1e-12));
    }
}

TEST_CASE("denoise: improves SNR on a noisy signal", "[v11w-e][wavelet][denoise]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize n = 1024;
    cont::Array<f64> clean(&alloc);
    cont::Array<f64> noisy(&alloc);
    clean.resize(n);
    noisy.resize(n);
    u64 s = 12345ULL;
    for (usize i = 0; i < n; ++i)
    {
        const f64 t = static_cast<f64>(i);
        clean[i] = std::sin(0.05 * t) + 0.5 * std::sin(0.011 * t); // smooth signal
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;  // deterministic uniform noise
        const f64 u = (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
        noisy[i] = clean[i] + 0.3 * u;
    }
    const auto w = wv::wavelet_by_name("db4");
    auto noise_err = [&](const cont::Array<f64>& x) {
        f64 e = 0.0;
        for (usize i = 0; i < n; ++i)
        {
            const f64 d = x[i] - clean[i];
            e += d * d;
        }
        return e;
    };
    const f64 err_noisy = noise_err(noisy);
    for (wv::DenoiseRule rule : {wv::DenoiseRule::VisuShrink, wv::DenoiseRule::BayesShrink, wv::DenoiseRule::SureShrink})
    {
        const auto den = wv::denoise<f64>(&alloc, cont::ConstSpan<f64>(noisy.data(), n), *w, 4, wv::ThresholdMode::Soft,
                                          rule);
        REQUIRE(den.size() == n);
        const f64 err_den = noise_err(den);
        INFO("rule " << static_cast<int>(rule) << " err_noisy " << err_noisy << " err_den " << err_den);
        CHECK(err_den < err_noisy); // denoising reduces the error vs the clean signal
    }
}

TEST_CASE("dwt2 + denoise: run-twice bit-identical (determinism moat)", "[v11w-e][wavelet][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const usize r = static_cast<usize>(ref_img_rows);
    const usize c = static_cast<usize>(ref_img_cols);
    const auto w = wv::wavelet_by_name("db2");
    const auto a = wv::dwt2<f64>(&alloc, ref_img, r, c, *w, Mode::Periodization);
    const auto b = wv::dwt2<f64>(&alloc, ref_img, r, c, *w, Mode::Periodization);
    REQUIRE(a.cA.size() == b.cA.size());
    CHECK(std::memcmp(a.cA.data(), b.cA.data(), a.cA.size() * sizeof(f64)) == 0);
    CHECK(std::memcmp(a.cD.data(), b.cD.data(), a.cD.size() * sizeof(f64)) == 0);
}

TEST_CASE("dwt2: {1,4,16}-thread bit-identical (2-D batched moat)", "[v11w-e][wavelet][dwt2][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    // a large enough image to engage the parallel path (>= 16×16).
    const usize r = 128;
    const usize c = 96;
    cont::Array<f64> img(&alloc);
    img.resize(r * c);
    u64 s = 4242ULL;
    for (usize i = 0; i < r * c; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        img[i] = (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0)) * 2.0 - 1.0;
    }
    const auto w = wv::wavelet_by_name("db4");
    cont::Array<f64> ref(&alloc);
    bool have = false;
    for (crd::u32 nw : {1U, 4U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            const auto d = wv::dwt2<f64>(&alloc, img.data(), r, c, *w, Mode::Symmetric);
            if (!have)
            {
                ref.resize(d.cD.size());
                for (usize i = 0; i < d.cD.size(); ++i)
                {
                    ref[i] = d.cD[i];
                }
                have = true;
            }
            else
            {
                INFO("threads " << nw);
                CHECK(std::memcmp(d.cD.data(), ref.data(), ref.size() * sizeof(f64)) == 0);
            }
        }
        crd::jobs::shutdown();
    }
}
