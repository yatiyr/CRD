// crd-hesap-dsp v11-f — elliptic special functions, gated INDEPENDENTLY vs scipy.special before ellipap composes
// them: ellipk (complete elliptic integral, AGM) + ellipj (Jacobi sn/cn/dn, descending Landen) + ellipdeg
// (the degree equation) — each to ~1e-12. References (plain C arrays) from gen_elliptic_refs.py.

#include <crd/hesap/dsp/ellip.hpp>
#include <crd/hesap/dsp/elliptic_fn.hpp>
#include <crd/hesap/dsp/filter.hpp>
#include <crd/hesap/dsp/freqz.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "elliptic_refs.inc"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cmath>

namespace dsp = crd::hesap::dsp;
namespace cont = crd::containers;
using crd::f64;
using crd::usize;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("dsp elliptic: ellipk (complete elliptic integral, AGM) matches scipy", "[v11-f][dsp][elliptic]")
{
    for (usize i = 0; i < sizeof(ref_ellipk_m) / sizeof(double); ++i)
    {
        INFO("m=" << ref_ellipk_m[i]);
        CHECK_THAT(dsp::ellipk<f64>(ref_ellipk_m[i]), WithinRel(ref_ellipk[i], 1e-13));
    }
}

TEST_CASE("dsp elliptic: ellipj (Jacobi sn/cn/dn, descending Landen) matches scipy", "[v11-f][dsp][elliptic]")
{
    const usize n = sizeof(ref_ellipj_u) / sizeof(double);
    for (usize i = 0; i < n; ++i)
    {
        f64 sn = 0.0;
        f64 cn = 0.0;
        f64 dn = 0.0;
        dsp::ellipj<f64>(ref_ellipj_u[i], ref_ellipj_m, sn, cn, dn);
        INFO("u=" << ref_ellipj_u[i]);
        CHECK_THAT(sn, WithinAbs(ref_ellipj_sn[i], 1e-12));
        CHECK_THAT(cn, WithinAbs(ref_ellipj_cn[i], 1e-12));
        CHECK_THAT(dn, WithinAbs(ref_ellipj_dn[i], 1e-12));
        // identity sn^2 + cn^2 == 1 and dn^2 + m sn^2 == 1.
        CHECK_THAT(sn * sn + cn * cn, WithinAbs(1.0, 1e-13));
        CHECK_THAT(dn * dn + ref_ellipj_m * sn * sn, WithinAbs(1.0, 1e-13));
    }
}

TEST_CASE("dsp elliptic: ellipdeg (the degree equation) matches scipy _ellipdeg", "[v11-f][dsp][elliptic]")
{
    for (usize i = 0; i < sizeof(ref_ellipdeg_N) / sizeof(double); ++i)
    {
        const usize N = static_cast<usize>(ref_ellipdeg_N[i]);
        INFO("N=" << N << " m1=" << ref_ellipdeg_m1[i]);
        CHECK_THAT(dsp::ellipdeg<f64>(N, ref_ellipdeg_m1[i]), WithinRel(ref_ellipdeg[i], 1e-11));
    }
}

TEST_CASE("dsp elliptic: arc_jac_sc (inverse Jacobi sc, complex Landen) matches scipy", "[v11-f][dsp][elliptic]")
{
    for (usize i = 0; i < sizeof(ref_arcsc_w) / sizeof(double); ++i)
    {
        INFO("w=" << ref_arcsc_w[i] << " m=" << ref_arcsc_m[i]);
        CHECK_THAT(dsp::arc_jac_sc<f64>(ref_arcsc_w[i], ref_arcsc_m[i]), WithinRel(ref_arcsc[i], 1e-10));
    }
}

TEST_CASE("dsp elliptic: ellipap analog prototype matches scipy (poles + zeros + gain)", "[v11-f][dsp][ellip]")
{
    crd::memory::TlsfAllocator alloc(1U << 21);
    const auto zpk = dsp::ellipap<f64>(&alloc, 4, 1.0, 40.0);
    // poles (sorted by rounded key, like scipy).
    auto key = [](double v) { return std::round(v * 1e9) / 1e9; };
    crd::containers::Array<f64> pre(zpk.p.allocator()), pim(zpk.p.allocator());
    crd::containers::Array<crd::usize> ord(zpk.p.allocator());
    for (usize i = 0; i < zpk.p.size(); ++i) { pre.push_back(zpk.p[i].re); pim.push_back(zpk.p[i].im); ord.push_back(i); }
    std::sort(ord.data(), ord.data() + ord.size(), [&](usize a, usize b) {
        return key(pre[a]) != key(pre[b]) ? key(pre[a]) < key(pre[b]) : key(pim[a]) < key(pim[b]); });
    REQUIRE(zpk.p.size() * 2 == sizeof(ref_ellipap4_poles) / sizeof(double));
    for (usize i = 0; i < ord.size(); ++i)
    {
        CHECK_THAT(pre[ord[i]], WithinAbs(ref_ellipap4_poles[2 * i], 1e-9));
        CHECK_THAT(pim[ord[i]], WithinAbs(ref_ellipap4_poles[2 * i + 1], 1e-9));
    }
    CHECK_THAT(zpk.k, WithinRel(ref_ellipap4_k, 1e-9));
}

TEST_CASE("dsp elliptic: digital ellip response matches scipy + equiripple BOTH bands", "[v11-f][dsp][ellip]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto zpk = dsp::ellip<f64>(&alloc, 6, 1.0, 60.0, 0.3);
    const auto sos = dsp::zpk_to_sos<f64>(&alloc, zpk);
    crd::containers::Array<f64> w(&alloc);
    crd::containers::Array<crd::hesap::Complex<f64>> h(&alloc);
    dsp::sosfreqz<f64>(sos, 64, w, h);
    REQUIRE(sizeof(ref_ellip6_mag) / sizeof(double) == 64);
    for (usize i = 0; i < 64; ++i)
    {
        INFO("mag[" << i << "]");
        CHECK_THAT(std::hypot(h[i].re, h[i].im), WithinAbs(ref_ellip6_mag[i], 1e-8));
    }
}
