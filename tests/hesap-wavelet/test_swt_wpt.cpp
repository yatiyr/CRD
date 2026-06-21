// crd-hesap-wavelet v11w-c — SWT (à trous) + iSWT + Wavelet Packets + best-basis.
// Gates: per-level SWT coefficients vs pywt + iswt vs pywt + iswt(swt(x))==x; WaveletPacket node data vs pywt +
// reconstruct round trip + best-basis cost optimality; run-twice bit-identical.

#include <crd/hesap/wavelet/swt.hpp>
#include <crd/hesap/wavelet/wpt.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstring>

namespace wv = crd::hesap::wavelet;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using Catch::Matchers::WithinAbs;
using Mode = wv::SignalExtensionMode;

namespace
{
#include "swt_wpt_refs.inc"

template <usize N> void check_arr(const double (&ref)[N], const cont::Array<f64>& got, f64 tol)
{
    REQUIRE(got.size() == N);
    for (usize i = 0; i < N; ++i)
    {
        INFO("idx " << i);
        CHECK_THAT(got[i], WithinAbs(ref[i], tol));
    }
}
template <usize N> void check_span(const double (&ref)[N], cont::ConstSpan<f64> got, f64 tol)
{
    REQUIRE(got.size() == N);
    for (usize i = 0; i < N; ++i)
    {
        INFO("idx " << i);
        CHECK_THAT(got[i], WithinAbs(ref[i], tol));
    }
}
} // namespace

TEST_CASE("swt: per-level coefficients vs pywt + iswt round trip", "[v11w-c][wavelet][swt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const cont::ConstSpan<f64> xs(ref_swt_input, sizeof(ref_swt_input) / sizeof(ref_swt_input[0]));

#define CHECK_SWT(WAV, WAVSAN)                                                                                          \
    do                                                                                                                  \
    {                                                                                                                   \
        const auto w = wv::wavelet_by_name(WAV);                                                                        \
        const auto c = wv::swt<f64>(&alloc, xs, *w, 3); /* coarse-first [L3,L2,L1] */                                   \
        REQUIRE(c.size() == 3);                                                                                         \
        INFO(WAV);                                                                                                      \
        check_arr(ref_swt_##WAVSAN##_L1_cA, c[2].cA, 1e-10); /* c[2] = level 1 */                                      \
        check_arr(ref_swt_##WAVSAN##_L1_cD, c[2].cD, 1e-10);                                                           \
        check_arr(ref_swt_##WAVSAN##_L2_cA, c[1].cA, 1e-10);                                                           \
        check_arr(ref_swt_##WAVSAN##_L2_cD, c[1].cD, 1e-10);                                                           \
        check_arr(ref_swt_##WAVSAN##_L3_cA, c[0].cA, 1e-10);                                                           \
        check_arr(ref_swt_##WAVSAN##_L3_cD, c[0].cD, 1e-10);                                                           \
        const auto r = wv::iswt<f64>(&alloc, c, *w);                                                                    \
        check_arr(ref_iswt_##WAVSAN, r, 1e-9); /* vs pywt.iswt */                                                      \
        for (usize i = 0; i < xs.size(); ++i)                                                                          \
        {                                                                                                              \
            INFO("recon " << i);                                                                                       \
            CHECK_THAT(r[i], WithinAbs(xs[i], 1e-9)); /* perfect reconstruction */                                    \
        }                                                                                                              \
    } while (0)

    CHECK_SWT("haar", haar);
    CHECK_SWT("db2", db2);
    CHECK_SWT("db4", db4);
    CHECK_SWT("sym3", sym3);
#undef CHECK_SWT
}

TEST_CASE("wpt: node coefficients vs pywt + reconstruction + best basis", "[v11w-c][wavelet][wpt]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const cont::ConstSpan<f64> xs(ref_swt_input, sizeof(ref_swt_input) / sizeof(ref_swt_input[0]));

#define CHECK_WP_NODE(WAVSAN, PATH) check_span(ref_wp_##WAVSAN##_##PATH, wp.node(#PATH), 1e-10)

    {
        const auto w = wv::wavelet_by_name("db2");
        wv::WaveletPacket<f64> wp(&alloc, xs, *w, Mode::Periodization, 3);
        CHECK_WP_NODE(db2, a);
        CHECK_WP_NODE(db2, d);
        CHECK_WP_NODE(db2, aa);
        CHECK_WP_NODE(db2, ad);
        CHECK_WP_NODE(db2, da);
        CHECK_WP_NODE(db2, dd);
        CHECK_WP_NODE(db2, aaa);
        CHECK_WP_NODE(db2, ddd);
        CHECK_WP_NODE(db2, ada);
        // reconstruction round trip
        const auto r = wp.reconstruct(&alloc, *w);
        REQUIRE(r.size() == xs.size());
        for (usize i = 0; i < xs.size(); ++i)
        {
            INFO("recon " << i);
            CHECK_THAT(r[i], WithinAbs(xs[i], 1e-9));
        }
        // best basis: optimal cost <= the full deepest-level cost (a fixed basis).
        cont::Array<usize> lv(&alloc), ix(&alloc);
        const f64 best = wp.best_basis(&alloc, lv, ix);
        REQUIRE(lv.size() == ix.size());
        REQUIRE(lv.size() >= 1);
        f64 level3_cost = 0.0;
        for (usize idx = 0; idx < 8; ++idx)
        {
            // cost of the 8 deepest nodes (a fixed full-depth basis)
            char path[4] = {0};
            for (int b = 0; b < 3; ++b)
            {
                path[b] = ((idx >> (2 - b)) & 1U) ? 'd' : 'a';
            }
            level3_cost += wv::shannon_entropy_cost<f64>(wp.node(cont::StringView(path, 3)));
        }
        CHECK(best <= level3_cost + 1e-9); // best-basis is optimal by construction
    }

    {
        const auto w = wv::wavelet_by_name("sym3");
        wv::WaveletPacket<f64> wp(&alloc, xs, *w, Mode::Periodization, 3);
        CHECK_WP_NODE(sym3, a);
        CHECK_WP_NODE(sym3, dd);
        CHECK_WP_NODE(sym3, ddd);
    }
#undef CHECK_WP_NODE
}

TEST_CASE("swt + wpt: run-twice bit-identical (determinism moat)", "[v11w-c][wavelet][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const cont::ConstSpan<f64> xs(ref_swt_input, sizeof(ref_swt_input) / sizeof(ref_swt_input[0]));
    const auto w = wv::wavelet_by_name("db4");
    const auto a = wv::swt<f64>(&alloc, xs, *w, 3);
    const auto b = wv::swt<f64>(&alloc, xs, *w, 3);
    for (usize l = 0; l < a.size(); ++l)
    {
        REQUIRE(a[l].cA.size() == b[l].cA.size());
        CHECK(std::memcmp(a[l].cA.data(), b[l].cA.data(), a[l].cA.size() * sizeof(f64)) == 0);
        CHECK(std::memcmp(a[l].cD.data(), b[l].cD.data(), a[l].cD.size() * sizeof(f64)) == 0);
    }
}
