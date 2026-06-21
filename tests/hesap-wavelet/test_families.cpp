// crd-hesap-wavelet v11w-a — families + filter banks.
// Gates: wavelet_by_name returns the pywt coefficients (the generated engine table vs an INDEPENDENT pywt read);
// the QMF builder reproduces the stored orthogonal bank; orthonormality (Σh=√2, Σh²=1, double-shift orthogonality).

#include <crd/containers/array.hpp>
#include <crd/hesap/wavelet/families.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

namespace wv = crd::hesap::wavelet;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using Catch::Matchers::WithinAbs;

namespace
{
#include "wavelet_refs.inc"

template <usize N> void check_span(const double (&ref)[N], cont::ConstSpan<double> got, f64 tol)
{
    REQUIRE(got.size() == N);
    for (usize i = 0; i < N; ++i)
    {
        INFO("tap " << i);
        CHECK_THAT(got[i], WithinAbs(ref[i], tol));
    }
}
} // namespace

TEST_CASE("wavelet families: lookup returns the pywt filter bank", "[v11w-a][wavelet][families]")
{
    const auto db4 = wv::wavelet_by_name("db4");
    REQUIRE(db4.has_value());
    CHECK(db4->orthogonal);
    CHECK(db4->len() == 8);
    check_span(ref_fb_db4_dec_lo, db4->dec_lo, 1e-15);
    check_span(ref_fb_db4_dec_hi, db4->dec_hi, 1e-15);
    check_span(ref_fb_db4_rec_lo, db4->rec_lo, 1e-15);
    check_span(ref_fb_db4_rec_hi, db4->rec_hi, 1e-15);

    const auto bior = wv::wavelet_by_name("bior2.2");
    REQUIRE(bior.has_value());
    CHECK_FALSE(bior->orthogonal);
    check_span(ref_fb_bior2_2_dec_lo, bior->dec_lo, 1e-15);
    check_span(ref_fb_bior2_2_rec_lo, bior->rec_lo, 1e-15);

    const auto rbio = wv::wavelet_by_name("rbio2.2");
    REQUIRE(rbio.has_value());
    check_span(ref_fb_rbio2_2_dec_hi, rbio->dec_hi, 1e-15);
    check_span(ref_fb_rbio2_2_rec_hi, rbio->rec_hi, 1e-15);

    CHECK_FALSE(wv::wavelet_by_name("not_a_wavelet").has_value());
    CHECK(wv::wavelet_count() > 50); // the full catalog
}

TEST_CASE("wavelet families: QMF builder reproduces the stored orthogonal bank", "[v11w-a][wavelet][families]")
{
    crd::memory::TlsfAllocator alloc(1U << 16);
    for (const char* name : {"haar", "db4", "sym4", "coif1"})
    {
        const auto w = wv::wavelet_by_name(name);
        REQUIRE(w.has_value());
        const usize l = w->len();
        cont::Array<f64> dl(&alloc), dh(&alloc), rl(&alloc), rh(&alloc), scaling(&alloc);
        dl.resize(l);
        dh.resize(l);
        rl.resize(l);
        rh.resize(l);
        scaling.resize(l);
        for (usize k = 0; k < l; ++k) // feed the stored synthesis low-pass (rec_lo) through the builder
        {
            scaling[k] = w->rec_lo[k];
        }
        wv::orthogonal_filter_bank<f64>(cont::ConstSpan<f64>(scaling.data(), l), cont::Span<f64>(dl.data(), l),
                                        cont::Span<f64>(dh.data(), l), cont::Span<f64>(rl.data(), l),
                                        cont::Span<f64>(rh.data(), l));
        INFO("wavelet " << name);
        for (usize k = 0; k < l; ++k)
        {
            CHECK_THAT(dl[k], WithinAbs(w->dec_lo[k], 1e-15));
            CHECK_THAT(dh[k], WithinAbs(w->dec_hi[k], 1e-15));
            CHECK_THAT(rl[k], WithinAbs(w->rec_lo[k], 1e-15));
            CHECK_THAT(rh[k], WithinAbs(w->rec_hi[k], 1e-15));
        }
    }
}

TEST_CASE("wavelet families: orthonormality of orthogonal scaling filters", "[v11w-a][wavelet][families]")
{
    const f64 sqrt2 = std::sqrt(2.0);
    for (const char* name : {"haar", "db2", "db4", "db8", "sym6", "coif3"})
    {
        const auto w = wv::wavelet_by_name(name);
        REQUIRE(w.has_value());
        REQUIRE(w->orthogonal);
        const usize l = w->len();
        f64 sum = 0.0, energy = 0.0;
        for (usize k = 0; k < l; ++k)
        {
            sum += w->dec_lo[k];
            energy += w->dec_lo[k] * w->dec_lo[k];
        }
        INFO("wavelet " << name);
        CHECK_THAT(sum, WithinAbs(sqrt2, 1e-12));   // Σ h = √2
        CHECK_THAT(energy, WithinAbs(1.0, 1e-12));  // Σ h² = 1
        // double-shift orthogonality: Σ_k h[k] h[k+2m] = δ_{m,0}.
        for (usize m = 1; m * 2 < l; ++m)
        {
            f64 dot = 0.0;
            for (usize k = 0; k + 2 * m < l; ++k)
            {
                dot += w->dec_lo[k] * w->dec_lo[k + 2 * m];
            }
            INFO("shift m=" << m);
            CHECK_THAT(dot, WithinAbs(0.0, 1e-12));
        }
    }
}
