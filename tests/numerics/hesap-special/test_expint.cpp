// crd-hesap-special v12-d — exponential & trigonometric integrals gated vs scipy.special + f32.

#include <crd/hesap/special/special.hpp>

#include "expint_refs.inc"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

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

// The cases below are the defects found in the 2026-11 review of this module: each failed before its fix. References
// are mpmath (50 digits) except where a scipy value is named; tolerances are the module's (1e-12 relative, 1e-13
// absolute), with no absolute floor where the value is tiny.
TEST_CASE("elliptic: F, E and Pi continue past |phi| = pi/2 (DLMF 19.2(ii))", "[v12-d][special][elliptic]")
{
    // {phi, m, F, E}
    static constexpr f64 kQuasi[][4] = {
        {2.0, 0.3, 2.2205905521284742, 1.8089647253633312},     {3.0, 0.7, 4.0088016697027201, 2.3420790345982354},
        {-2.5, -0.5, -2.2086345320136657, -2.8422085784965443}, {4.0, 0.95, 6.7958507204742785, 2.8834983405893999},
        {7.5, 0.2, 7.9043426349564425, 7.126959654155092},      {-1.7, 0.9, -2.9771039239118107, -1.1466304711509987},
        {10.0, 0.5, 11.715622315665893, 8.6638861065257423},    {3.5, -2.0, 2.6877488067028074, 4.7417282933338632},
    };
    for (const auto& row : kQuasi)
    {
        INFO("phi=" << row[0] << " m=" << row[1]);
        CHECK(close(sf::ellint_f(row[0], row[1]), row[2], 1e-12, 1e-13));
        CHECK(close(sf::ellint_e_inc(row[0], row[1]), row[3], 1e-12, 1e-13));
    }
    // {n, phi, m, Pi}: the last three have n sin^2(phi) > 1 somewhere on the path, the Cauchy principal value
    static constexpr f64 kPi[][4] = {
        {0.3, 2.0, 0.5, 3.0736257122682814},   {-0.4, 3.5, 0.7, 3.7804794223813131},
        {0.6, -2.2, 0.2, -4.1497749441430203}, {0.5, 4.0, -1.0, 4.4826254381096825},
        {1.7, 1.0, 0.5, 1.2273317645497457},   {2.5, 2.6, 0.3, -1.0212972552738602},
        {1.7, -4.0, 0.5, -1.9945724579308352},
    };
    for (const auto& row : kPi)
    {
        INFO("n=" << row[0] << " phi=" << row[1] << " m=" << row[2]);
        CHECK(close(sf::ellint_pi(row[0], row[1], row[2]), row[3], 1e-12, 1e-13));
    }
    // the reflection F(pi - phi) = 2K - F(phi), also on the doubles next to pi/2 where K may be infinite
    const f64 inf = std::numeric_limits<f64>::infinity();
    CHECK(close(sf::ellint_f(2.0, 0.3) + sf::ellint_f(3.14159265358979323846 - 2.0, 0.3), 2.0 * sf::ellint_k(0.3),
                1e-12, 1e-13));
    CHECK(sf::ellint_f(1.5707963267948968, 1.0) == inf);
    CHECK(sf::ellint_f(-3.0, 1.0) == -inf);
    CHECK(sf::ellint_e_inc(3.0, 1.0) > 1.0);
    // m = 1: the complete integral Pi diverges, so Pi(n, phi, 1) is infinite past pi/2 whatever the sign of n
    CHECK(sf::ellint_pi(0.3, 2.0, 1.0) == inf);
    CHECK(sf::ellint_pi(-1.0, 3.0, 1.0) == inf);
    CHECK(sf::ellint_pi(0.0, -3.0, 1.0) == -inf);
    // n > 1 at m = 1: the integrand 1/((1 - n sin^2)|cos|) is negative at pi/2, so the complete integral is -inf and
    // Pi is -inf past pi/2 (+inf for negative phi)
    CHECK(sf::ellint_pi(2.0, 3.0, 1.0) == -inf);
    CHECK(sf::ellint_pi(2.0, -3.0, 1.0) == inf);
    // n = 1: the integrand has the pole 1/cos^2 at pi/2, which is not integrable and positive
    CHECK(sf::ellint_pi(1.0, 3.0, 0.5) == inf);
    CHECK(sf::ellint_pi(1.0, 3.0, 1.0) == inf);
    CHECK(sf::ellint_pi(1.0, -3.0, 0.5) == -inf);
    // phi = +-inf: every half turn adds the complete integral, with the sign of that integral (negative for n > 1)
    for (const f64 m : {-2.0, 0.0, 0.5, 1.0})
    {
        INFO("m=" << m);
        CHECK(sf::ellint_pi(0.0, inf, m) == inf);
        CHECK(sf::ellint_pi(0.0, -inf, m) == -inf);
        CHECK(sf::ellint_pi(0.5, inf, m) == inf);
        CHECK(sf::ellint_pi(0.5, -inf, m) == -inf);
    }
    CHECK(sf::ellint_pi(2.0, inf, 0.5) == -inf);
    CHECK(sf::ellint_pi(2.0, -inf, 0.5) == inf);
    CHECK(sf::ellint_pi(2.0, inf, 1.0) == -inf);
    CHECK(std::isnan(sf::ellint_pi(0.5, inf, 2.0)));
    CHECK(std::isnan(sf::ellint_pi(0.5, std::numeric_limits<f64>::quiet_NaN(), 0.5)));
}

TEST_CASE("elliptic: complete K and E at m = 1 and m = -inf", "[v12-d][special][elliptic]")
{
    const f64 inf = std::numeric_limits<f64>::infinity();
    CHECK(sf::ellint_k(1.0) == inf);
    CHECK(sf::ellint_e(1.0) == 1.0);
    CHECK(sf::ellint_k(-inf) == 0.0);
    CHECK(sf::ellint_e(-inf) == inf);
    CHECK(std::isnan(sf::ellint_k(1.5)));
    CHECK(std::isnan(sf::ellint_e(std::numeric_limits<f64>::quiet_NaN())));
}

TEST_CASE("elliptic: R_J for a negative last argument is the Cauchy principal value (DLMF 19.20.14)",
          "[v12-d][special][elliptic]")
{
    // {x, y, z, p, R_J}
    static constexpr f64 kPv[][5] = {
        {0.5, 2.0, 3.0, -0.7, -0.086742497967208829},
        {1.0, 1.0, 1.0, -1.0, -0.56516213978965423},
        {0.0, 1.0, 1.0, -1.0, -2.3561944901923449},
        {0.2, 0.7, 5.0, -2.0, -0.53618694509495762},
        {3.0, 1.0, 2.0, -50.0, -0.040428626995041892},
        {0.001, 4.0, 9.0, -0.001, 9.5246330284495023},
        // far beyond the others in size, or the products (z-y)(y-x) of the relation leave the double range
        {0.0, 1e-300, 1e-100, -0.1, -6.9493441098157333e+53},
        {0.0, 1e-300, 1e+100, -1e+100, -1.3838402631593664e-147},
        {1.0, 2.0, 3.0, -1e+50, -2.1808378064067244e-50},
        {0.0, 1e-300, 1e+300, -1e+100, -1.3836304973381072e-247},
    };
    for (const auto& row : kPv)
    {
        INFO("rj x=" << row[0] << " y=" << row[1] << " z=" << row[2] << " p=" << row[3]);
        CHECK(close(sf::elliprj(row[0], row[1], row[2], row[3]), row[4], 1e-12, 1e-13));
    }
    // y - p beyond the double range: the value (about 1e-462) underflows to zero, in every order of the arguments
    static constexpr f64 kHuge[][4] = {
        {1.0, 1e+308, 1.7e+308, -1e+308},
        {1e+308, 1.5e+308, 1.7e+308, -1e+308},
        {1e+308, 1.5e+308, 1.7e+308, -1.7e+308},
    };
    for (const auto& row : kHuge)
    {
        INFO("rj x=" << row[0] << " y=" << row[1] << " z=" << row[2] << " p=" << row[3]);
        CHECK(sf::elliprj(row[0], row[1], row[2], row[3]) == 0.0);
        CHECK(sf::elliprj(row[2], row[0], row[1], row[3]) == 0.0);
        CHECK(sf::elliprj(row[1], row[2], row[0], row[3]) == 0.0);
    }
}

TEST_CASE("elliptic: R_J over the double range (alpha and beta of the duplication leave it)",
          "[v12-d][special][elliptic]")
{
    // {x, y, z, p, R_J}: values that are normal doubles though the products p(p+lambda)^2 overflow or underflow
    static constexpr f64 kWide[][5] = {
        {1e-300, 1e-300, 1e-300, 1.0, 3.0e+150},
        {1.0, 3.7, 1e-6, 9.5e307, 3.4815553096885689e-308},
        {1e-300, 1e-300, 1e100, 1.0, 1.0361632918473205e-47},
        {0.5, 2.0, 1e-6, 1e250, 4.5716630807861136e-250},
        {1e150, 3e150, 2e150, 1e150, 5.5911221740544415e-226},
        {1e-6, 1e-6, 1e-100, 1e-300, 6.9285496943989357e+58},
        {1e-30, 1e-30, 1e6, 1e-300, 9.3254696266258842e+29},
        // the same iteration far from 1, where it needs no second formulation: every argument at 1e-100 or 1e101-1e102,
        // and a p that is tiny beside the others
        {1e-70, 1.5e-70, 2e-70, 3e-70, 3.5821748724960522e+104},
        {1e-100, 2e-100, 3e-100, 4e-100, 2.3984809974956775e+149},
        {1e+101, 2e+101, 3e+101, 4e+101, 7.5846628767189522e-153},
        {1e+102, 2e+102, 3e+102, 4e+102, 2.3984809974956777e-154},
        {1.0, 1.5, 3.0, 1e-90, 146.41385362288102},
        {1.0, 1.5, 3.0, 1e-250, 406.92161898530452},
    };
    for (const auto& row : kWide)
    {
        INFO("rj x=" << row[0] << " y=" << row[1] << " z=" << row[2] << " p=" << row[3]);
        CHECK(close(sf::elliprj(row[0], row[1], row[2], row[3]), row[4], 1e-12, 0.0));
    }
    // arguments above 6e307, where the duplication's sums 3x overflow (R_C(x, x) = 1/sqrt(x) is 7.67e-155)
    const f64 top = 1.7e308;
    CHECK(close(sf::elliprf(top, top, top), 7.6696498884737045e-155, 1e-12, 0.0));
    CHECK(close(sf::elliprf(1.0, 2.0, top), 2.7255507432712383e-152, 1e-12, 0.0));
    CHECK(sf::elliprj(top, top, top, top) == 0.0);
    const f64 inf = std::numeric_limits<f64>::infinity();
    CHECK(sf::elliprj(1e-300, 1e-300, 1e-300, 1e-300) == inf); // 10^450
    CHECK(sf::elliprj(1e300, 1e300, 1e300, 1e300) == 0.0);     // 10^-450
}

TEST_CASE("elliptic: Carlson integrals with infinite, vanishing or negative arguments", "[v12-d][special][elliptic]")
{
    const f64 inf = std::numeric_limits<f64>::infinity();
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    // diverging integrals are +inf
    CHECK(sf::elliprf(0.0, 0.0, 1.0) == inf);
    CHECK(sf::elliprf(0.0, 0.0, 0.0) == inf);
    CHECK(sf::elliprd(0.0, 0.0, 1.0) == inf);
    CHECK(sf::elliprd(1.0, 2.0, 0.0) == inf);
    CHECK(sf::elliprj(0.0, 0.0, 2.0, 1.0) == inf);
    CHECK(sf::elliprj(0.0, 0.0, 2.0, -1.0) == inf);
    // an infinite argument gives 0
    CHECK(sf::elliprf(inf, 1.0, 2.0) == 0.0);
    CHECK(sf::elliprf(1.0, 2.0, inf) == 0.0);
    CHECK(sf::elliprj(1.0, 2.0, 3.0, inf) == 0.0);
    CHECK(sf::elliprj(1.0, inf, 3.0, 2.0) == 0.0);
    CHECK(sf::elliprj(1.0, 2.0, 3.0, -inf) == 0.0);
    // NaN and negative arguments stay undefined
    CHECK(std::isnan(sf::elliprf(-1.0, 1.0, 2.0)));
    CHECK(std::isnan(sf::elliprf(nan, 1.0, 2.0)));
    CHECK(std::isnan(sf::elliprd(1.0, 2.0, -3.0)));
    CHECK(std::isnan(sf::elliprc(-1.0, 1.0)));
    CHECK(std::isnan(sf::elliprj(1.0, 2.0, 3.0, 0.0)));
    CHECK(std::isnan(sf::elliprj(-1.0, 2.0, 3.0, 1.0)));
}

TEST_CASE("elliptic: R_F, R_C and R_D over the whole double range", "[v12-d][special][elliptic]")
{
    const f64 inf = std::numeric_limits<f64>::infinity();
    // {x, y, z, R_F}: arguments from 10^306 to the top of the range (the sums of the duplication overflow, and a mean
    // above 1.1e307 must not leave the iteration after one step), then the smallest subnormals, alone and beside the
    // top
    static constexpr f64 kRf[][4] = {
        {4.9999999999999999e+306, 2.9999999999999998e+307, 5.0000000000000001e+307, 2.0625913171787623e-154},
        {2e+307, 2.9999999999999998e+307, 2.5e+307, 2.008136495085719e-154},
        {1.4999999999999999e+307, 3.9999999999999999e+307, 7.9999999999999999e+307, 1.5842048996427866e-154},
        {9.9999999999999999e+306, 9.9999999999999999e+306, 9.9999999999999999e+306, 3.1622776601683795e-154},
        {0.0, 4.9406564584124654e-324, 1.6999999999999999e+308, 5.5871079273982159e-152},
        {1.6999999999999999e+308, 0.0, 4.9406564584124654e-324, 5.5871079273982159e-152},
        {4.9406564584124654e-324, 4.9406564584124654e-324, 4.9406564584124654e-324, 4.4989137945431964e+161},
        {9.9998886718268301e-321, 4.9406564584124654e-324, 4.9406564584124654e-324, 4.5005761407796352e+160},
        {4.9406564584124654e-324, 9.9999999999999694e-311, 4.9406564584124654e-324, 1.6012493727173902e+156},
        {4.9406564584124654e-324, 4.9406564584124654e-324, 1.6999999999999999e+308, 5.5817917312021383e-152},
        {9.9999999999999694e-311, 1.6999999999999999e+308, 1.6999999999999999e+308, 1.2047457872617384e-154},
        {1e-295, 2e-295, 3e-295, 2.2988048918835327e+147},
        {3e-305, 7e-305, 5e-305, 1.4385056104366608e+152},
    };
    for (const auto& row : kRf)
    {
        INFO("rf x=" << row[0] << " y=" << row[1] << " z=" << row[2]);
        CHECK(close(sf::elliprf(row[0], row[1], row[2]), row[3], 1e-12, 0.0));
    }
    // {x, y, R_C}: R_C(x, x) = 1/sqrt(x) is 1.414e-154 at 5e307 and 4.5e161 at the smallest subnormal
    static constexpr f64 kRc[][3] = {
        {5.0000000000000001e+307, 5.0000000000000001e+307, 1.414213562373095e-154},
        {1.6999999999999999e+308, 1.0000000000000001e+300, 7.7991148874206252e-154},
        {1.0000000000000001e+300, 1.6999999999999999e+308, 1.2046869672754659e-154},
        {4.9406564584124654e-324, 4.9406564584124654e-324, 4.4989137945431964e+161},
        {9.9998886718268301e-321, 4.9406564584124654e-324, 4.5005761407796352e+160},
        {9.9999999999999999e+306, -9.9999999999999999e+306, 1.9708112541485246e-154},
        {4.9406564584124654e-324, -4.9406564584124654e-324, 2.8038366299743792e+161},
        {2.9999999999999998e+307, 1e-300, 1.2781599360037016e-151},
        // a mean of 1e-295 or 1e-305 is a normal number: the loop closes and is not repeated, the value is the same
        {1e-295, 2e-295, 2.4836470664490252e+147},
        {1e-295, -2e-295, 1.2022125790504759e+147},
        {3e-305, 7e-305, 1.3551497369217337e+152},
    };
    for (const auto& row : kRc)
    {
        INFO("rc x=" << row[0] << " y=" << row[1]);
        CHECK(close(sf::elliprc(row[0], row[1]), row[2], 1e-12, 0.0));
    }
    // {x, y, z, R_D}: two huge arguments beside a vanishing third, the subnormals, and the overflow of R_D(x, x, x)
    static constexpr f64 kRd[][4] = {
        {1.6999999999999999e+308, 1.6999999999999999e+308, 4.9406564584124654e-324, 7.9392596374291703e-147},
        {1.6999999999999999e+308, 1.6999999999999999e+308, 9.9998886718268301e-321, 1.7647157055090642e-148},
        {9.9999999999999999e+306, 2.9999999999999998e+307, 1e-300, 1.7320508075688773e-157},
        {9.9999999999999694e-311, 9.9999999999999694e-311, 1.0, 1069.7815097839111},
        {4.9406564584124654e-324, 1.0, 1.0, 2.3561944901923448},
    };
    for (const auto& row : kRd)
    {
        INFO("rd x=" << row[0] << " y=" << row[1] << " z=" << row[2]);
        CHECK(close(sf::elliprd(row[0], row[1], row[2]), row[3], 1e-12, 0.0));
    }
    CHECK(sf::elliprd(4.9406564584124654e-324, 4.9406564584124654e-324, 4.9406564584124654e-324) == inf);
    CHECK(sf::elliprd(9.9998886718268301e-321, 9.9998886718268301e-321, 9.9998886718268301e-321) == inf);
    // an infinite argument gives 0 for R_C and R_D too, and R_D with z = -0 diverges like R_D with z = +0
    CHECK(sf::elliprc(inf, 1.0) == 0.0);
    CHECK(sf::elliprc(1.0, inf) == 0.0);
    CHECK(sf::elliprc(inf, inf) == 0.0);
    CHECK(sf::elliprc(1.0, -inf) == 0.0);
    CHECK(sf::elliprd(1.0, 2.0, inf) == 0.0);
    CHECK(sf::elliprd(inf, 2.0, 3.0) == 0.0);
    CHECK(sf::elliprd(0.0, 2.0, inf) == 0.0);
    CHECK(sf::elliprd(1.0, 2.0, -0.0) == inf);
    CHECK(sf::elliprd(1.0, 2.0, 0.0) == inf);
    // R_C(x, 0) diverges like R_D(x, y, 0)
    CHECK(sf::elliprc(1.0, 0.0) == inf);
    CHECK(sf::elliprc(0.0, 0.0) == inf);
    CHECK(sf::elliprc(2.0, -0.0) == inf);
}

TEST_CASE("elliptic: complete K and E up to m = 1 (the polynomials of the Cody form stop at 1 - m = 0.01)",
          "[v12-d][special][elliptic]")
{
    // {m, K, E}: 1 - m from 3.2e-5 to the double next to 1, and both sides of the change of method at 1 - m = 0.01
    static constexpr f64 kKe[][3] = {
        {0.99996799999999997, 6.5612261789320012, 1.0000969799587869},
        {0.99999998999999995, 10.59663475457528, 1.0000000504831741},
        {0.99999999999, 14.050512331249584, 1.0000000000677525},
        {0.99999999999999989, 19.754694645958441, 1.0000000000000011},
        {0.99000010000000005, 3.6956423076469962, 1.015993409689556},
        {0.98999990000000004, 3.6956324183826381, 1.0159936803606489},
    };
    for (const auto& row : kKe)
    {
        INFO("m=" << row[0]);
        CHECK(close(sf::ellint_k(row[0]), row[1], 1e-12, 1e-13));
        CHECK(close(sf::ellint_e(row[0]), row[2], 1e-12, 1e-13));
    }
    // {phi, m, F, E}: beyond pi/2 the quasi-period adds 2K and 2E, whose errors then add to the result's
    static constexpr f64 kInc[][4] = {
        {1.6000000000000001, 0.99999899999999997, 12.361857944827262, 1.0004325776017802},
        {2.5, 0.99999998999999995, 20.502506193990264, 1.4015279564009357},
        {4.0, 0.99999999999, 29.089713053476274, 2.7568024954445929},
        {-1.7, 0.99999950000000004, -14.543135546085143, -1.0083388232469059},
    };
    for (const auto& row : kInc)
    {
        INFO("phi=" << row[0] << " m=" << row[1]);
        CHECK(close(sf::ellint_f(row[0], row[1]), row[2], 1e-12, 1e-13));
        CHECK(close(sf::ellint_e_inc(row[0], row[1]), row[3], 1e-12, 1e-13));
    }
}

TEST_CASE("elliptic: F and E at phi = +-inf and at m = -inf", "[v12-d][special][elliptic]")
{
    const f64 inf = std::numeric_limits<f64>::infinity();
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    // F and E grow by 2K and 2E for every half turn, so they are infinite for an infinite phi (odd in phi)
    for (const f64 m : {-1e300, -2.0, 0.0, 0.5, 1.0})
    {
        INFO("m=" << m);
        CHECK(sf::ellint_f(inf, m) == inf);
        CHECK(sf::ellint_f(-inf, m) == -inf);
        CHECK(sf::ellint_e_inc(inf, m) == inf);
        CHECK(sf::ellint_e_inc(-inf, m) == -inf);
    }
    // m = -inf: K = 0 and F(phi, -inf) = 0; the integrand of E grows without bound
    CHECK(sf::ellint_f(0.0, -inf) == 0.0);
    CHECK(sf::ellint_f(1.0, -inf) == 0.0);
    CHECK(sf::ellint_f(3.0, -inf) == 0.0);
    CHECK(sf::ellint_e_inc(0.0, -inf) == 0.0);
    CHECK(sf::ellint_e_inc(1.0, -inf) == inf);
    CHECK(sf::ellint_e_inc(-3.0, -inf) == -inf);
    CHECK(std::isnan(sf::ellint_f(inf, -inf)));
    CHECK(std::isnan(sf::ellint_f(nan, 0.5)));
    CHECK(std::isnan(sf::ellint_e_inc(0.5, nan)));
    CHECK(std::isnan(sf::ellint_f(inf, 2.0)));
}

TEST_CASE("elliptic: R_J principal value where the terms of the relation cancel (z >> y, p near -y)",
          "[v12-d][special][elliptic]")
{
    // {x, y, z, p, R_J}: the result is a part in z/y of the terms of DLMF 19.20.14, with x = y or x close to y
    static constexpr f64 kCancel[][5] = {
        {9.9999999999999995e-21, 9.9999999999999995e-21, 1.0, -9.9999999999999995e-21, -34.828497165750605},
        {1.0, 1.0, 1e+20, -1.0, -3.4828497165750601e-29},
        {1.0, 1.0, 1000000000000000.0, -1.0, -8.2832078273696596e-22},
        {9.9999999999999995e-07, 9.9999999999999995e-07, 3.7000000000000002, -9.9999999999999995e-07,
         -1.6344601328743127},
        {1.0, 1.0000000010000001, 1000000000000.0, -1.0, 3.5398704413879024e-16},
        {0.001, 0.001, 1000.0, -0.001, -0.00033682537822287643},
        {2.0, 2.0, 3000000000.0, -2.0, -9.8202205712385398e-14},
        {1.0, 1.0, 10000000.0, -1.0000005000000001, -5.1002132577518401e-10},
        {3.9999999999999998e-07, 4.0000001199999997e-07, 0.0050000000000000001, -3.9999999999999998e-07,
         -20830.497761860839},
        {9.0, 9.0, 1000000.0, -9.0, -9.0034352567578987e-09},
        // the whole range of the arguments: a ratio z/y beyond 1e240, subnormal arguments, a pair that is only nearly
        // equal
        {9.9999999999999998e-121, 9.9999999999999998e-121, 9.9999999999999998e+119, -9.9999999999999998e-121,
         -4.1475503750976813e-178},
        {9.9999999999999998e-201, 9.9999999999999998e-201, 1e+100, -9.9999999999999998e-201, -5.1837136669450019e-148},
        {4.9406564584124654e-324, 4.9406564584124654e-324, 1.0, -4.9406564584124654e-324, -558.61977471187583},
        {1.0, 1.0000000010000001, 1e+308, -1.0, 3.7500003074638911e-164},
        {300000.0, 300000.00030000001, 2.9999999999999999e+295, -300000.0, 2.2821774296241654e-163},
    };
    for (const auto& row : kCancel)
    {
        INFO("rj x=" << row[0] << " y=" << row[1] << " z=" << row[2] << " p=" << row[3]);
        CHECK(close(sf::elliprj(row[0], row[1], row[2], row[3]), row[4], 1e-12, 0.0));
        CHECK(close(sf::elliprj(row[2], row[0], row[1], row[3]), row[4], 1e-12, 0.0)); // symmetric in x, y, z
    }
    // the result beyond the double range keeps its sign
    CHECK(sf::elliprj(1e-300, 1e-300, 1e-280, -1e-300) == -std::numeric_limits<f64>::infinity());
}

// Near phi = +-pi/2 (and its odd multiples) with m or n near 1, 1 - m sin^2(phi) cancels: a rounded sin^2 near 1 is
// subtracted from 1, and at the double nearest pi/2 it is 0 where cos^2(phi) is not (F(that phi, 1) came out 38.718,
// and Pi(1, that phi, 0.5) was NaN, the pole of R_J). Values from mpmath at 60 digits at the exact double inputs;
// where n sin^2(phi) > 1, the real part of mpmath's complex ellippi, the Cauchy principal value.
TEST_CASE("elliptic: F, E and Pi near phi = pi/2 with m or n near 1", "[v12-d][special][elliptic]")
{
    // {phi, m, F, E}: the last four past pi/2, on the doubles nearest 3 pi/2 and 5 pi/2, where the rounded phi/pi
    // names the neighbouring half turn
    static constexpr f64 kNear[][4] = {
        {1.5707963267948966, 1.0, 38.025003373828866, 1.0},
        {-1.5707963267948966, 1.0, -38.025003373828866, -1.0},
        {1.5707963267948, 1.0, 30.660820794748968, 1.0},
        {1.570796, 1.0, 15.627080271110785, 0.9999999999999466},
        {1.5707, 1.0, 9.94091121365546, 0.9999999953605743},
        {1.57, 1.0, 7.828648037733774, 0.9999996829318346},
        {1.570796, 0.999999999999, 14.880571355345033, 1.0000000000070182},
        {1.5707, 0.999999999999, 9.94088427235288, 0.9999999953650447},
        {1.5707, 0.9999999, 9.145254601078321, 1.0000004163411913},
        {-1.5707, 0.9999999, -9.145254601078321, -1.0000004163411913},
        {4.71238898038469, 0.999999999999, 45.605447940026664, 3.000000000022052},
        {-4.71238898038469, 0.999999999999, -45.605447940026664, -3.000000000022052},
        {7.853981633974483, 0.999999999999, 76.00907990004444, 5.000000000036754},
        {4.71238898038469, 0.9999999, 28.3360271939868, 3.0000013418013745},
    };
    // {Pi(0.3, phi, m)} at the {phi, m} of each row above
    static constexpr f64 kNearPi[] = {
        53.84012453751562,  -53.84012453751562,  43.31986371025862,  21.84309153363268,  13.719992882965505,
        10.702474251503116, 20.776650225400022,  13.719954395393092, 12.583340816462773, -12.583340816462773,
        63.706713353330855, -63.706713353330855, 106.17785558888475, 39.03611300592067,
    };
    static_assert(len(kNear) == len(kNearPi));
    for (std::size_t i = 0; i < len(kNear); ++i)
    {
        const auto& row = kNear[i];
        INFO("phi=" << row[0] << " m=" << row[1]);
        CHECK(close(sf::ellint_f(row[0], row[1]), row[2], 1e-12, 1e-13));
        CHECK(close(sf::ellint_e_inc(row[0], row[1]), row[3], 1e-12, 1e-13));
        CHECK(close(sf::ellint_pi(0.3, row[0], row[1]), kNearPi[i], 1e-12, 1e-13));
    }
    // {n, phi, m, Pi}: n = 1 (1 - n sin^2 was the pole 0 of R_J at the first three) and n near 1 on both sides
    static constexpr f64 kNearN[][4] = {
        {1.0, 1.5707963267948966, 0.5, 2.3095860183650104e+16},
        {1.0, -1.5707963267948966, 0.5, -2.3095860183650104e+16},
        {1.0, 1.5707963267948, 0.5, 14632222078160.371},
        {1.0, 1.57079632679, 0.5, 288816072323.16486},
        {1.0, 1.570796, 0.5, 4327525.6146576265},
        {1.0, 1.5707, 0.5, 14680.566884391386},
        {1.0, 1.57, 0.5, 1775.074080665082},
        {0.9999999, 1.5707, 0.5, 5701.634798123095},
        {0.9999999, 1.570796, 0.5, 7019.346294793767},
        {1.0000001, 1.5707, 0.5, 1406.0669757138107},
        {1.0000001, 1.57, 0.5, 1878.3927800803745},
        {1.0000001, -1.5707, 0.5, -1406.0669757138107},
        {0.9999999, 4.71238898038469, 0.5, 21071.90361269435},
    };
    for (const auto& row : kNearN)
    {
        INFO("n=" << row[0] << " phi=" << row[1] << " m=" << row[2]);
        CHECK(close(sf::ellint_pi(row[0], row[1], row[2]), row[3], 1e-12, 1e-13));
    }
}

// The edge of the real domain for m > 1, sin^2(phi) = 1/m: 1 - m sin^2(phi) is within an ulp of 0 and its rounding
// made it negative (NaN) at these inputs. F and Pi have an infinite derivative in phi there, so a rounding of the
// argument moves them by about its square root (1e-8 relative); E has a finite one and holds the module limit. And
// Pi(2, phi, m) at the double nearest pi/4, where 1 - 2 sin^2(phi) = cos(2 phi) = 6.1e-17 and the cheap forms give 0,
// the pole of R_J (NaN). Values from mpmath at 60 digits at the exact doubles.
TEST_CASE("elliptic: F, E and Pi at the edge of the domain for m > 1 and at the pole for n = 2",
          "[v12-d][special][elliptic]")
{
    // {n, phi, m, F, E, Pi}
    static constexpr f64 kEdge[][6] = {
        {0.7695803858029941, 0.8000996602392672, 1.942881692827097, 1.339241614657019, 0.6095122980999521,
         1.762836897566225},
        {-0.4574675729976416, 1.1254950004925997, 1.2277895871829725, 2.0679560945217372, 0.8238444506618001,
         1.7097301181712683},
        {-0.8386869105032556, 0.816738998914119, 1.8821038247924684, 1.371501519603868, 0.6212698899587188,
         1.1212400581253046},
        {-0.5944603629927258, -0.9614909570597364, 1.4870440529168967, -1.6701669033468696, -0.7204108881455579,
         -1.3796985219999118},
    };
    for (const auto& row : kEdge)
    {
        INFO("n=" << row[0] << " phi=" << row[1] << " m=" << row[2]);
        CHECK(close(sf::ellint_f(row[1], row[2]), row[3], 1e-7, 0.0));
        CHECK(close(sf::ellint_e_inc(row[1], row[2]), row[4], 1e-12, 1e-13));
        CHECK(close(sf::ellint_pi(row[0], row[1], row[2]), row[5], 1e-7, 0.0));
    }
    // {phi, m, Pi(2, phi, m)}
    static constexpr f64 kPole[][3] = {
        {0.7853981633974483, 0.7408416310133681, 23.73003728978681},
        {0.7853981633974483, 0.9136880963351484, 25.467575969688916},
        {0.7853981633974483, 0.21348374788980548, 20.069401999771134},
        {0.7853981633974483, 0.03998161321349682, 19.197422851763086},
        {-0.7853981633974483, 0.25056667635707847, -20.272136360787982},
    };
    for (const auto& row : kPole)
    {
        INFO("phi=" << row[0] << " m=" << row[1]);
        CHECK(close(sf::ellint_pi(2.0, row[0], row[1]), row[2], 1e-12, 1e-13));
    }
}

TEST_CASE("expint: the limits E0(0), E1(0), Ei(+inf), Si/Ci at infinity, and Ei up to its overflow",
          "[v12-d][special][expint]")
{
    const f64 inf = std::numeric_limits<f64>::infinity();
    CHECK(sf::expint_en(0, 0.0) == inf);
    CHECK(sf::expint_en(1, 0.0) == inf);
    CHECK(sf::expint_e1(0.0) == inf);
    CHECK(sf::expint_en(2, 0.0) == 1.0);
    CHECK(std::isnan(sf::expint_e1(-1.0)));
    CHECK(sf::expint_ei(inf) == inf);
    CHECK(sf::sinint(inf) == 1.5707963267948966);
    CHECK(sf::sinint(-inf) == -1.5707963267948966);
    CHECK(sf::cosint(inf) == 0.0);
    // Ei is finite until e^x/x passes the largest double, near 716.3; e^x alone overflows at 709.78
    static constexpr f64 kEi[][2] = {{700.0, 1.4509787360525609e+301}, {709.5, 1.9124756899200398e+305},
                                     {710.0, 3.1509156882062012e+305}, {711.0, 8.5530133216252952e+305},
                                     {713.0, 6.3021170119194961e+306}, {715.0, 4.6436256703705711e+307},
                                     {716.0, 1.2605029106040894e+308}};
    for (const auto& row : kEi)
    {
        INFO("Ei x=" << row[0]);
        CHECK(close(sf::expint_ei(row[0]), row[1], 1e-12, 0.0));
    }
    CHECK(sf::expint_ei(717.0) == inf);
}

TEST_CASE("fresnel: the limits at infinity and the phase at large x", "[v12-d][special][fresnel]")
{
    const f64 inf = std::numeric_limits<f64>::infinity();
    CHECK(sf::fresnel_s(inf) == 0.5);
    CHECK(sf::fresnel_c(inf) == 0.5);
    CHECK(sf::fresnel_s(-inf) == -0.5);
    CHECK(sf::fresnel_c(-1e200) == -0.5);
    CHECK(sf::fresnel_s(1e154) == 0.5);
    CHECK(std::isnan(sf::fresnel_s(std::numeric_limits<f64>::quiet_NaN())));
    // {x, S, C}: the phase pi x^2/2 of these x is far beyond 1e5 radians; the values are mpmath's
    static constexpr f64 kBig[][3] = {
        {100000.5, 0.49999705921481548, 0.50000121811310735},
        {5000000.25, 0.49999993664457575, 0.50000000623996464},
        {38820343.41164018, 0.49999999632613288, 0.4999999926695465},
        {100000000.0000151, 0.50000000012367949, 0.49999999681930483},
        {9000000000000.5, 0.49999999999996732, 0.50000000000001353},
    };
    for (const auto& row : kBig)
    {
        INFO("fresnel x=" << row[0]);
        CHECK(close(sf::fresnel_s(row[0]), row[1], 1e-12, 1e-13));
        CHECK(close(sf::fresnel_c(row[0]), row[2], 1e-12, 1e-13));
        CHECK(close(sf::fresnel_s(-row[0]), -row[1], 1e-12, 1e-13));
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
        // The original bar for H, kept: the series (x<16, ~1e-10 at worst near 16), the Y_ν + Gauss–Laguerre integral
        // and the downward recurrence (16 ≤ x ≲ 30) and the asymptotic (x ≳ 30) sit near 1e-12; the reference's own
        // error is ~1e-13 at x ≈ 25.
        CHECK(close(sf::struve_h(ref_st_nu[i], ref_st_x[i]), ref_sth_v[i], 1e-9, 1e-11));
    }
    for (std::size_t i = 0; i < len(ref_stl_v); ++i)
    {
        INFO("struve_l nu=" << ref_stl_nu[i] << " x=" << ref_stl_x[i]);
        CHECK(close(sf::struve_l(ref_stl_nu[i], ref_stl_x[i]), ref_stl_v[i], 1e-11, 1e-12));
    }
}

TEST_CASE("struve: L near and at a negative half-integer order and where its series leaves the doubles vs mpmath",
          "[v12-d][special][struve]")
{
    // mpmath at 70 digits at the exact double inputs (the series at two precisions, 120 digits apart). Base gave NaN at
    // the half-integers (the first term of the series from m = n is (x/2)^(n+1/2)/Gamma(n+3/2), whose Gamma leaves the
    // doubles: L_{-(n+1/2)} = I_{n+1/2}), 0 where (x/2)^(nu+1) underflowed before the division by Gamma(nu+3/2), and up
    // to 25 units near a half-integer, where gamma()'s reflection rounds pi*s before its sine.
    const f64 points[][3] = {
        {-195.5, 126.30268007428117, 0.0000674236932140666074994461847762},
        {-249.5, 41.819286468447004, 7.43837344850871253438729020733e-162},
        {-170.17738140799065, 195.74828158456478, 9.0298909536749690784421309944e+52},
        {-144.32170498496956, 389.78005650242773, 1.24356853184060715182409039983e+156},
        {-145.50006684872037, 47.2009330548983, -2.71519530676573504126850266045e+46},
        {-271.5030234552658, 15.346050873877855, -8.23347298302780547824957604596e+298},
        {-88.500263776103935, 3.7645240133126632, 5.64531263423886111039387332885e+104},
        {-131.50811528547783, 81.509828020038839, -1834186.33259741378083731768681},
        // within 3e-6 and 3e-8 of a half-integer: the terms after m = -nu - 3/2 grow back, and the sum had stopped
        {-60.500003162045992, 32.261575084764324, 94.6550093379151193399461772875},
        {-29.50000003014614, 10.898644359893616, -5.61875550688469059562875452723},
        // x near 600: the sum of ratios needs some 630 terms, base stopped at 500 (3% off)
        {-293.95634216199767, 594.62600449858087, 3.00191572020197791814932069987e+225},
        {-287.49984948195555, 599.62865079557901, 1.62059801622892326368160910494e+229},
    };
    for (const auto& p : points)
    {
        INFO("struve_l nu=" << p[0] << " x=" << p[1]);
        CHECK(close(sf::struve_l(p[0], p[1]), p[2], 1e-12, 1e-13));
    }
}

TEST_CASE("struve: H near a negative half-integer order vs mpmath", "[v12-d][special][struve]")
{
    // mpmath at 70 digits at the exact double inputs. Base: 1/Gamma of a negative argument near a pole came from
    // gamma()'s reflection, which rounds pi*s before the sine: in the series' first term (up to 2e5 units), in the
    // asymptotic expansion (2.5e3 units at nu = -99.5000000105, x = 42.9).
    const f64 points[][3] = {
        {-91.500007860686168, 6.3139736630249486, -9.28946676415859120636755688967e+87},
        {-199.50000031860145, 19.225895782164866, -7.8526590637408405062338320358e+168},
        {-4.4999999727631792, 5.4165969027422385, 0.370715175014298012127247104589},
        {-91.500000037855088, 0.6571875555903639, -3.50907560346855712867853396129e+174},
        {-99.499999989506179, 42.862295149014663, 42619317165744613.1214176356634},
        {-108.49999998793189, 47.363940684787991, -2437848533502144472.94615129512},
    };
    for (const auto& p : points)
    {
        INFO("struve_h nu=" << p[0] << " x=" << p[1]);
        CHECK(close(sf::struve_h(p[0], p[1]), p[2], 1e-12, 1e-13));
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
