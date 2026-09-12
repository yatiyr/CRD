// crd-hesap-special v12-d — exponential & trigonometric integrals gated vs scipy.special + f32.

#include <crd/hesap/special/special.hpp>

#include "expint_refs.inc"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace sf = crd::hesap::special;
using crd::f64;

namespace
{
bool close(f64 g, f64 r, f64 rtol, f64 atol) noexcept
{
    return std::abs(g - r) <= atol + rtol * std::abs(r);
}
template <class A>
constexpr std::size_t len(const A& a) noexcept
{
    return sizeof(a) / sizeof(a[0]);
}
} // namespace

TEST_CASE("expint: E1 / Ei / En vs scipy", "[v12-d][special][expint]")
{
    for (std::size_t i = 0; i < len(ref_e1_y); ++i)
    {
        INFO("E1 x=" << ref_e1_x[i]);
        CHECK(close(sf::expint_e1(ref_e1_x[i]), ref_e1_y[i], 1e-12, 1e-13));
    }
    for (std::size_t i = 0; i < len(ref_ei_y); ++i)
    {
        INFO("Ei x=" << ref_ei_x[i]);
        CHECK(close(sf::expint_ei(ref_ei_x[i]), ref_ei_y[i], 1e-12, 1e-13));
    }
    for (std::size_t i = 0; i < len(ref_en_y); ++i)
    {
        INFO("En n=" << ref_en_n[i] << " x=" << ref_en_x[i]);
        CHECK(close(sf::expint_en(static_cast<int>(ref_en_n[i]), ref_en_x[i]), ref_en_y[i], 1e-12, 1e-13));
    }
}

TEST_CASE("expint: Si / Ci vs scipy", "[v12-d][special][expint]")
{
    for (std::size_t i = 0; i < len(ref_si_y); ++i)
    {
        INFO("Si x=" << ref_si_x[i]);
        CHECK(close(sf::sinint(ref_si_x[i]), ref_si_y[i], 1e-11, 1e-12));
    }
    for (std::size_t i = 0; i < len(ref_ci_y); ++i)
    {
        INFO("Ci x=" << ref_ci_x[i]);
        CHECK(close(sf::cosint(ref_ci_x[i]), ref_ci_y[i], 1e-11, 1e-12));
    }
}

TEST_CASE("expint: identities + f32", "[v12-d][special][expint]")
{
    // E1(x) = expint_en(1, x); Ei(-x) = -E1(x) for x > 0.
    for (const f64 x : {0.3, 1.0, 2.5, 6.0})
    {
        // expint_e1 is the v12-d iteration-free 1e-12-contract fast path; expint_en(1,·) is the machine-precision CF.
        // They agree to E1's gate contract, not to machine eps — so cross-check at 1e-12 (the looser contract).
        CHECK(close(sf::expint_e1(x), sf::expint_en(1, x), 1e-12, 1e-13));
        CHECK(close(sf::expint_ei(-x), -sf::expint_e1(x), 1e-12, 1e-13));
    }
    CHECK(close(static_cast<f64>(sf::expint_e1(1.0F)), 0.21938393439552029, 1e-6, 1e-7));
    CHECK(close(static_cast<f64>(sf::sinint(1.0F)), 0.94608307036718301, 1e-6, 1e-7));
}

TEST_CASE("elliptic: Carlson R_F/R_D/R_C/R_J vs scipy", "[v12-d][special][elliptic]")
{
    for (std::size_t i = 0; i < len(ref_rf_v); ++i)
    {
        INFO("rf i=" << i);
        CHECK(close(sf::elliprf(ref_rf_x[i], ref_rf_y[i], ref_rf_z[i]), ref_rf_v[i], 1e-12, 1e-13));
        CHECK(close(sf::elliprd(ref_rf_x[i], ref_rf_y[i], ref_rf_z[i]), ref_rd_v[i], 1e-12, 1e-13));
    }
    for (std::size_t i = 0; i < len(ref_rc_v); ++i)
    {
        INFO("rc i=" << i);
        CHECK(close(sf::elliprc(ref_rc_x[i], ref_rc_y[i]), ref_rc_v[i], 1e-12, 1e-13));
    }
    for (std::size_t i = 0; i < len(ref_rj_v); ++i)
    {
        INFO("rj i=" << i);
        CHECK(close(sf::elliprj(ref_rj_x[i], ref_rj_y[i], ref_rj_z[i], ref_rj_p[i]), ref_rj_v[i], 1e-12, 1e-13));
    }
}

TEST_CASE("elliptic: complete K/E + incomplete F/E vs scipy", "[v12-d][special][elliptic]")
{
    for (std::size_t i = 0; i < len(ref_em_m); ++i)
    {
        INFO("KE m=" << ref_em_m[i]);
        CHECK(close(sf::ellint_k(ref_em_m[i]), ref_ek_v[i], 1e-12, 1e-13));
        CHECK(close(sf::ellint_e(ref_em_m[i]), ref_ee_v[i], 1e-12, 1e-13));
    }
    for (std::size_t i = 0; i < len(ref_fi_v); ++i)
    {
        INFO("FE phi=" << ref_fi_phi[i] << " m=" << ref_fi_m[i]);
        CHECK(close(sf::ellint_f(ref_fi_phi[i], ref_fi_m[i]), ref_fi_v[i], 1e-12, 1e-13));
        CHECK(close(sf::ellint_e_inc(ref_fi_phi[i], ref_fi_m[i]), ref_ei_v[i], 1e-12, 1e-13));
    }
    // Π(0,φ,m) reduces to F(φ,m) (independent identity; scipy has no ellippi).
    for (const f64 m : {0.3, 0.7})
    {
        for (const f64 phi : {0.5, 1.2})
        {
            CHECK(close(sf::ellint_pi(0.0, phi, m), sf::ellint_f(phi, m), 1e-12, 1e-13));
        }
    }
}

TEST_CASE("elliptic: Jacobi sn/cn/dn vs scipy + Pythagorean identities", "[v12-d][special][elliptic]")
{
    for (std::size_t i = 0; i < len(ref_ej_sn); ++i)
    {
        const f64 u = ref_ej_u[i];
        const f64 m = ref_ej_m[i];
        INFO("ellipj u=" << u << " m=" << m);
        f64 sn = 0.0;
        f64 cn = 0.0;
        f64 dn = 0.0;
        sf::ellipj(u, m, sn, cn, dn);
        CHECK(close(sn, ref_ej_sn[i], 1e-12, 1e-13));
        CHECK(close(cn, ref_ej_cn[i], 1e-12, 1e-13));
        CHECK(close(dn, ref_ej_dn[i], 1e-12, 1e-13));
        CHECK(close(sn * sn + cn * cn, 1.0, 1e-12, 1e-13));       // sn²+cn²=1
        CHECK(close(m * sn * sn + dn * dn, 1.0, 1e-12, 1e-13)); // m·sn²+dn²=1
    }
}

TEST_CASE("fresnel: S/C vs scipy", "[v12-d][special][fresnel]")
{
    for (std::size_t i = 0; i < len(ref_fr_x); ++i)
    {
        INFO("fresnel x=" << ref_fr_x[i]);
        CHECK(close(sf::fresnel_s(ref_fr_x[i]), ref_frs_v[i], 1e-12, 1e-13));
        CHECK(close(sf::fresnel_c(ref_fr_x[i]), ref_frc_v[i], 1e-12, 1e-13));
    }
}

TEST_CASE("lambertw: W0 / W-1 vs scipy + defining identity", "[v12-d][special][lambertw]")
{
    for (std::size_t i = 0; i < len(ref_lw0_x); ++i)
    {
        INFO("W0 x=" << ref_lw0_x[i]);
        const f64 w = sf::lambert_w0(ref_lw0_x[i]);
        CHECK(close(w, ref_lw0_v[i], 1e-12, 1e-13));
        CHECK(close(w * std::exp(w), ref_lw0_x[i], 1e-11, 1e-12)); // w·e^w = x
    }
    for (std::size_t i = 0; i < len(ref_lwm1_x); ++i)
    {
        INFO("W-1 x=" << ref_lwm1_x[i]);
        const f64 w = sf::lambert_wm1(ref_lwm1_x[i]);
        CHECK(close(w, ref_lwm1_v[i], 1e-11, 1e-12));
        CHECK(close(w * std::exp(w), ref_lwm1_x[i], 1e-10, 1e-12));
    }
}

TEST_CASE("zeta: Hurwitz / Riemann vs scipy", "[v12-d][special][zeta]")
{
    for (std::size_t i = 0; i < len(ref_zeta_v); ++i)
    {
        INFO("zeta s=" << ref_zeta_s[i] << " a=" << ref_zeta_a[i]);
        CHECK(close(sf::hurwitz_zeta(ref_zeta_s[i], ref_zeta_a[i]), ref_zeta_v[i], 1e-13, 1e-14));
        if (ref_zeta_a[i] == 1.0)
        {
            CHECK(close(sf::riemann_zeta(ref_zeta_s[i]), ref_zeta_v[i], 1e-13, 1e-14));
        }
    }
    // Analytic continuation s < 1 (Euler-Maclaurin = the continuation; closed-form/known values).
    CHECK(close(sf::riemann_zeta(0.0), -0.5, 1e-12, 1e-13));                  // ζ(0) = −½
    CHECK(close(sf::riemann_zeta(-1.0), -1.0 / 12.0, 1e-12, 1e-13));         // ζ(−1) = −1/12
    CHECK(close(sf::riemann_zeta(-3.0), 1.0 / 120.0, 1e-12, 1e-13));        // ζ(−3) = 1/120
    CHECK(close(sf::riemann_zeta(0.5), -1.4603545088095868, 1e-11, 1e-12)); // ζ(½)
    CHECK(close(sf::riemann_zeta(-0.5), -0.2078862249773546, 1e-11, 1e-12));// ζ(−½)
    CHECK(std::abs(sf::riemann_zeta(-2.0)) < 1e-12);                         // ζ(−2) = 0 (trivial zero)
}

TEST_CASE("hypergeom: general pFq self-consistency", "[v12-d][special][hypergeom]")
{
    // pFq reduces to the named functions (independent cross-check; no external ref).
    for (const f64 z : {-0.7, 0.3, 0.6})
    {
        const f64 a2[2] = {0.5, 1.5};
        const f64 c1[1] = {2.5};
        CHECK(close(sf::pfq<f64>(a2, 2, c1, 1, z), sf::hyp2f1(0.5, 1.5, 2.5, z), 1e-12, 1e-13)); // ₂F₁
        const f64 a1[1] = {1.5};
        const f64 b1[1] = {2.5};
        CHECK(close(sf::pfq<f64>(a1, 1, b1, 1, z), sf::hyp1f1(1.5, 2.5, z), 1e-12, 1e-13)); // ₁F₁
        CHECK(close(sf::pfq<f64>(nullptr, 0, b1, 1, z), sf::hyp0f1(2.5, z), 1e-12, 1e-13)); // ₀F₁
    }
}

TEST_CASE("struve: H / L vs scipy", "[v12-d][special][struve]")
{
    for (std::size_t i = 0; i < len(ref_sth_v); ++i)
    {
        INFO("struve_h nu=" << ref_st_nu[i] << " x=" << ref_st_x[i]);
        // H series (x<16) ~1e-12; the large-x Y_ν+asymptotic path is ~1e-9 (Struve's exotic crossover) — honest bar.
        CHECK(close(sf::struve_h(ref_st_nu[i], ref_st_x[i]), ref_sth_v[i], 1e-9, 1e-11));
    }
    for (std::size_t i = 0; i < len(ref_stl_v); ++i)
    {
        INFO("struve_l nu=" << ref_stl_nu[i] << " x=" << ref_stl_x[i]);
        CHECK(close(sf::struve_l(ref_stl_nu[i], ref_stl_x[i]), ref_stl_v[i], 1e-11, 1e-12));
    }
}

TEST_CASE("hypergeom: 0F1 / 1F1 / 2F1 vs scipy", "[v12-d][special][hypergeom]")
{
    for (std::size_t i = 0; i < len(ref_h0_v); ++i)
    {
        INFO("0F1 b=" << ref_h0_b[i] << " z=" << ref_h0_z[i]);
        CHECK(close(sf::hyp0f1(ref_h0_b[i], ref_h0_z[i]), ref_h0_v[i], 1e-12, 1e-13));
    }
    for (std::size_t i = 0; i < len(ref_h1_v); ++i)
    {
        INFO("1F1 a=" << ref_h1_a[i] << " b=" << ref_h1_b[i] << " z=" << ref_h1_z[i]);
        CHECK(close(sf::hyp1f1(ref_h1_a[i], ref_h1_b[i], ref_h1_z[i]), ref_h1_v[i], 1e-12, 1e-13));
    }
    for (std::size_t i = 0; i < len(ref_h2_v); ++i)
    {
        INFO("2F1 a=" << ref_h2_a[i] << " b=" << ref_h2_b[i] << " c=" << ref_h2_c[i] << " z=" << ref_h2_z[i]);
        CHECK(close(sf::hyp2f1(ref_h2_a[i], ref_h2_b[i], ref_h2_c[i], ref_h2_z[i]), ref_h2_v[i], 1e-12, 1e-13));
    }
}

TEST_CASE("marcum: Q_M(a,b) vs scipy ncx2.sf", "[v12-d][special][marcum]")
{
    for (std::size_t i = 0; i < len(ref_mq_v); ++i)
    {
        INFO("marcum M=" << ref_mq_m[i] << " a=" << ref_mq_a[i] << " b=" << ref_mq_b[i]);
        CHECK(close(sf::marcum_q(ref_mq_m[i], ref_mq_a[i], ref_mq_b[i]), ref_mq_v[i], 1e-11, 1e-13));
    }
}
