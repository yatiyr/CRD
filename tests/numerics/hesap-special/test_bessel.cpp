// crd-hesap-special v12-b — Bessel / Airy / Kelvin gated vs scipy.special + Wronskian self-checks + f32 sanity.

#include "bessel_refs.inc"

#include <crd/hesap/special/special.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <limits>

namespace sf = crd::hesap::special;
using crd::f64;

namespace
{
bool close(f64 got, f64 ref, f64 rtol, f64 atol) noexcept
{
    return std::abs(got - ref) <= atol + rtol * std::abs(ref);
}
// A reference beyond the doubles is written as the largest double and must come back as an infinity of that sign.
bool tiny_close(f64 got, f64 ref) noexcept
{
    if (std::abs(ref) == std::numeric_limits<f64>::max())
    {
        return std::isinf(got) && std::signbit(got) == std::signbit(ref);
    }
    return close(got, ref, 1e-12, 1e-300);
}
bool close_c(std::complex<f64> got, std::complex<f64> ref, f64 rtol, f64 atol) noexcept
{
    return std::abs(got - ref) <= atol + rtol * std::abs(ref);
}
template <class A> constexpr std::size_t len(const A& a) noexcept
{
    return sizeof(a) / sizeof(a[0]);
}
} // namespace

TEST_CASE("bessel: cyl J/Y + derivatives vs scipy", "[v12-b][special][bessel]")
{
    for (std::size_t i = 0; i < len(ref_besj_y); ++i)
    {
        const f64 nu = ref_besj_nu[i];
        const f64 x = ref_besj_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_j(nu, x), ref_besj_y[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_neumann(nu, x), ref_besy_y[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_bessel_j_prime(nu, x), ref_besjp_y[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_neumann_prime(nu, x), ref_besyp_y[i], 1e-12, 1e-13));
    }
}

TEST_CASE("bessel: cyl I/K + derivatives vs scipy", "[v12-b][special][bessel]")
{
    for (std::size_t i = 0; i < len(ref_besi_y); ++i)
    {
        const f64 nu = ref_besi_nu[i];
        const f64 x = ref_besi_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_i(nu, x), ref_besi_y[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_bessel_k(nu, x), ref_besk_y[i], 1e-12, 1e-300));
        CHECK(close(sf::cyl_bessel_i_prime(nu, x), ref_besip_y[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_bessel_k_prime(nu, x), ref_beskp_y[i], 1e-12, 1e-300));
    }
}

TEST_CASE("bessel: negative order J/Y/I (reflection) vs scipy", "[v12-b][special][bessel]")
{
    for (std::size_t i = 0; i < len(ref_besjn_y); ++i)
    {
        const f64 nu = ref_besjn_nu[i];
        const f64 x = ref_besjn_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_j(nu, x), ref_besjn_y[i], 1e-11, 1e-12));
        CHECK(close(sf::cyl_neumann(nu, x), ref_besyn_y[i], 1e-11, 1e-12));
        CHECK(close(sf::cyl_bessel_i(nu, x), ref_besin_y[i], 1e-11, 1e-12));
    }
}

TEST_CASE("bessel: negative order derivatives vs scipy", "[v12-b][special][bessel]")
{
    // The reflection coefficients do not depend on x, so J', Y', I' and K' at order −ν follow from the order-ν cores.
    for (std::size_t i = 0; i < len(ref_besjpn_y); ++i)
    {
        const f64 nu = ref_besjn_nu[i];
        const f64 x = ref_besjn_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_j_prime(nu, x), ref_besjpn_y[i], 1e-11, 1e-12));
        CHECK(close(sf::cyl_neumann_prime(nu, x), ref_besypn_y[i], 1e-11, 1e-12));
        CHECK(close(sf::cyl_bessel_i_prime(nu, x), ref_besipn_y[i], 1e-11, 1e-12));
        CHECK(close(sf::cyl_bessel_k_prime(nu, x), ref_beskpn_y[i], 1e-11, 1e-300));
    }
}

TEST_CASE("bessel: negative integer and half-integer order at small x", "[v12-b][special][bessel]")
{
    // sin(νπ) and cos(νπ) must be exact here: a residue of 1e-16 multiplies a Y_ν or K_ν of 1e9 and swamps the result.
    for (std::size_t i = 0; i < len(ref_besjs_y); ++i)
    {
        const f64 nu = ref_besjs_nu[i];
        const f64 x = ref_besjs_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_j(nu, x), ref_besjs_y[i], 1e-11, 1e-300));
        CHECK(close(sf::cyl_neumann(nu, x), ref_besys_y[i], 1e-11, 1e-300));
        CHECK(close(sf::cyl_bessel_i(nu, x), ref_besis_y[i], 1e-11, 1e-300));
    }
}

TEST_CASE("bessel: J at large order and argument vs scipy", "[v12-b][special][bessel]")
{
    // The Hankel asymptotic needs x ≫ ν²/2; x ≥ 2ν is not enough (ν = 100, x = 208 was wrong by 16).
    for (std::size_t i = 0; i < len(ref_besjl_y); ++i)
    {
        const f64 nu = ref_besjl_nu[i];
        const f64 x = ref_besjl_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_j(nu, x), ref_besjl_y[i], 1e-12, 1e-13));
    }
}

TEST_CASE("bessel: J, Y and their slopes at large argument vs scipy", "[v12-b][special][bessel]")
{
    // The continued fraction of the Steed method needs about x steps (its cap is 1e4), and ω = x − (ν/2 + ¼)π loses
    // ε·x of phase: from x = 1e3 the Hankel expansion with an exactly reduced phase takes over.
    for (std::size_t i = 0; i < len(ref_besb_j); ++i)
    {
        const f64 nu = ref_besb_nu[i];
        const f64 x = ref_besb_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_j(nu, x), ref_besb_j[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_neumann(nu, x), ref_besb_y[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_bessel_j_prime(nu, x), ref_besb_jp[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_neumann_prime(nu, x), ref_besb_yp[i], 1e-12, 1e-13));
    }
}

TEST_CASE("bessel: J, Y and their slopes at order above 100 and argument above 9000 vs mpmath",
          "[v12-b][special][bessel]")
{
    // The continued fraction needs about x steps (its cap is 1e4) and Hankel's expansion at the order itself needs
    // x ≫ ν²/2: the expansion at the base order and the recurrence in order (stable while ν ≤ x) serve instead.
    for (std::size_t i = 0; i < len(ref_besh_j); ++i)
    {
        const f64 nu = ref_besh_nu[i];
        const f64 x = ref_besh_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_j(nu, x), ref_besh_j[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_neumann(nu, x), ref_besh_y[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_bessel_j_prime(nu, x), ref_besh_jp[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_neumann_prime(nu, x), ref_besh_yp[i], 1e-12, 1e-13));
    }
}

TEST_CASE("bessel: small argument J, Y, I, K and slopes vs scipy", "[v12-b][special][bessel]")
{
    // Below x = 1e-3 J, J', I and I' come from their ascending series: the CF1 recurrence lost digits as x fell and its
    // 1e-300 seed then underflowed (I_{1/2}(1e-20) was +inf, I' was 3e5 times over the limit at 1e-10, J'_0(1e-35)
    // was 0).
    for (std::size_t i = 0; i < len(ref_bessm_j); ++i)
    {
        const f64 nu = ref_bessm_nu[i];
        const f64 x = ref_bessm_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_j(nu, x), ref_bessm_j[i], 1e-12, 1e-300));
        CHECK(close(sf::cyl_bessel_j_prime(nu, x), ref_bessm_jp[i], 1e-12, 1e-300));
        CHECK(close(sf::cyl_bessel_i(nu, x), ref_bessm_i[i], 1e-12, 1e-300));
        CHECK(close(sf::cyl_bessel_i_prime(nu, x), ref_bessm_ip[i], 1e-12, 1e-300));
    }
    for (std::size_t i = 0; i < len(ref_bessmy_y); ++i)
    {
        const f64 nu = ref_bessmy_nu[i];
        const f64 x = ref_bessmy_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_neumann(nu, x), ref_bessmy_y[i], 1e-12, 1e-300));
        CHECK(close(sf::cyl_neumann_prime(nu, x), ref_bessmy_yp[i], 1e-12, 1e-300));
        CHECK(close(sf::cyl_bessel_k(nu, x), ref_bessmy_k[i], 1e-12, 1e-300));
        CHECK(close(sf::cyl_bessel_k_prime(nu, x), ref_bessmy_kp[i], 1e-12, 1e-300));
    }
}

TEST_CASE("bessel: J' and I' that are subnormal are rounded once", "[v12-b][special][bessel]")
{
    // J'_ν(x) and I'_ν(x) at x = 1e-50, ν = 7.3 are about 1000 times the smallest subnormal: the leading term must not
    // be rounded to that grid more than once (three steps off is 0.3 %). The reference is rounded to the nearest
    // double.
    constexpr f64 step = std::numeric_limits<f64>::denorm_min();
    for (std::size_t i = 0; i < len(ref_bessd_jp); ++i)
    {
        const f64 nu = ref_bessd_nu[i];
        const f64 x = ref_bessd_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(std::abs(sf::cyl_bessel_j_prime(nu, x) - ref_bessd_jp[i]) <= step);
        CHECK(std::abs(sf::cyl_bessel_i_prime(nu, x) - ref_bessd_ip[i]) <= step);
    }
}

TEST_CASE("bessel: K' is -0 where it underflows", "[v12-b][special][bessel]")
{
    // K'_ν(x) = −(K_{ν−1} + K_{ν+1})/2 is negative for every x > 0, so a slope that underflows is −0, for either sign
    // of the order.
    for (const f64 nu : {0.0, 0.5, -0.5, 2.0, -1.5})
    {
        for (const f64 x : {1e10, 1e100})
        {
            INFO("nu=" << nu << " x=" << x);
            const f64 slope = sf::cyl_bessel_k_prime(nu, x);
            CHECK(slope == 0.0);
            CHECK(std::signbit(slope));
        }
    }
}

TEST_CASE("bessel: Hankel functions of real argument are J +- iY", "[v12-b][special][bessel]")
{
    // The pair comes from one core (with the reflection for a negative order); it must agree with J and Y taken apart.
    for (const f64 nu : {0.0, 0.5, 2.7, -0.5, -2.5, -3.0})
    {
        for (const f64 x : {0.3, 2.0, 9.5, 20.0, 40.0, 1500.0})
        {
            INFO("nu=" << nu << " x=" << x);
            const std::complex<f64> h1 = sf::cyl_hankel_1(nu, x);
            const std::complex<f64> h2 = sf::cyl_hankel_2(nu, x);
            CHECK(close(h1.real(), sf::cyl_bessel_j(nu, x), 1e-11, 1e-12));
            CHECK(close(h1.imag(), sf::cyl_neumann(nu, x), 1e-11, 1e-12));
            CHECK(h2.real() == h1.real());
            CHECK(h2.imag() == -h1.imag());
        }
    }
}

TEST_CASE("bessel: overflow gives an infinity, not NaN", "[v12-b][special][bessel]")
{
    // Y_ν(x) = −∞ and K_ν(x) = +∞ once the upward recurrence overflows, and the slopes follow. For an integer or
    // half-integer negative order an exactly zero reflection coefficient drops the overflowed term: Y_{−100.5} =
    // J_{100.5} is tiny (it underflows to 0 here) and J_{−100} = J_100.
    const f64 inf = std::numeric_limits<f64>::infinity();
    CHECK(sf::cyl_neumann(100.3, 1e-3) == -inf);
    CHECK(sf::cyl_neumann_prime(100.3, 1e-3) == inf);
    CHECK(sf::cyl_bessel_k(100.3, 1e-3) == inf);
    CHECK(sf::cyl_bessel_k_prime(100.3, 1e-3) == -inf);
    CHECK(sf::cyl_neumann(-100.0, 1e-3) == -inf);
    CHECK(sf::cyl_bessel_i(2.0, 1e100) == inf); // e^x overflows long before this
    CHECK(sf::cyl_bessel_i_prime(2.0, 1e100) == inf);
    CHECK(sf::cyl_bessel_i(1e5, 700.0) == 0.0); // an order far above x: underflow, not overflow
    CHECK(sf::cyl_bessel_i_prime(1e5, 745.0) == 0.0);
    CHECK(std::isnan(sf::cyl_neumann(0.0, -1.0))); // a negative argument is a domain error, not an overflow
    CHECK(std::isnan(sf::cyl_bessel_k_prime(0.0, -1.0)));
    for (const f64 x : {1e-3, 1e-2, 0.05})
    {
        INFO("x=" << x);
        CHECK(std::isfinite(sf::cyl_neumann(-100.5, x)));
        CHECK(std::isfinite(sf::cyl_bessel_j(-100.0, x)));
    }
    for (const f64 x : {0.5, 2.0, 5.0})
    {
        INFO("x=" << x);
        CHECK(close(sf::cyl_neumann(-100.5, x), sf::cyl_bessel_j(100.5, x), 1e-10, 1e-300));
        CHECK(close(sf::cyl_bessel_j(-100.0, x), sf::cyl_bessel_j(100.0, x), 1e-10, 1e-300));
    }
}

TEST_CASE("bessel: I and I' are continuous across the edges of the range that calls the core",
          "[v12-b][special][bessel]")
{
    // The core serves 0 <= nu <= 100 and 1e-3 <= x <= 512; outside it the series (x), the overflow classes (x, nu) and
    // the reflection (nu < 0) answer. A step across an edge is one rounding of the argument, so the two sides must
    // agree.
    const f64 eps = 1e-9;
    for (const f64 nu : {0.0, 0.5, 1.0, 2.7, 12.3, 60.0, 99.9})
    {
        for (const f64 edge : {1e-3, 512.0})
        {
            INFO("nu=" << nu << " x=" << edge);
            const f64 below = edge * (1.0 - eps);
            const f64 above = edge * (1.0 + eps);
            CHECK(
                close(sf::cyl_bessel_i(nu, below), sf::cyl_bessel_i(nu, above), 4.0 * eps * (nu + 1.0 + edge), 1e-300));
            CHECK(close(sf::cyl_bessel_i_prime(nu, below), sf::cyl_bessel_i_prime(nu, above),
                        4.0 * eps * (nu + 1.0 + edge), 1e-300));
        }
    }
    for (const f64 x : {0.05, 2.0, 30.0, 300.0})
    {
        INFO("x=" << x);
        const f64 above = std::nextafter(100.0, 200.0);
        CHECK(close(sf::cyl_bessel_i(100.0, x), sf::cyl_bessel_i(above, x), 1e-9, 1e-300));
        CHECK(close(sf::cyl_bessel_i_prime(100.0, x), sf::cyl_bessel_i_prime(above, x), 1e-9, 1e-300));
        CHECK(std::isfinite(sf::cyl_bessel_i(150.0, x)));
        CHECK(std::isfinite(sf::cyl_bessel_i_prime(150.0, x)));
    }
    CHECK(sf::cyl_bessel_i(300.0, 1.5) == 0.0); // an order far above x underflows: 0, not NaN
    CHECK(sf::cyl_bessel_i(300.0, 1.0) == 0.0); // also at x = 1 and below it
    CHECK(sf::cyl_bessel_i_prime(150.0, 0.05) == 0.0);
    CHECK(sf::cyl_bessel_i_prime(300.0, 1.5) == 0.0);
    CHECK(sf::cyl_bessel_i(0.0, 800.0) == std::numeric_limits<f64>::infinity());
    CHECK(sf::cyl_bessel_i_prime(0.0, 800.0) == std::numeric_limits<f64>::infinity());
}

TEST_CASE("bessel: spherical j_n / y_n vs scipy", "[v12-b][special][bessel]")
{
    for (std::size_t i = 0; i < len(ref_sphj_y); ++i)
    {
        const int n = static_cast<int>(ref_sphn[i]);
        const f64 x = ref_sphx[i];
        INFO("n=" << n << " x=" << x);
        CHECK(close(sf::sph_bessel(n, x), ref_sphj_y[i], 1e-11, 1e-12));
        CHECK(close(sf::sph_neumann(n, x), ref_sphy_y[i], 1e-11, 1e-12));
    }
}

TEST_CASE("bessel: Airy Ai/Bi + derivatives vs scipy", "[v12-b][special][airy]")
{
    for (std::size_t i = 0; i < len(ref_airy_x); ++i)
    {
        const f64 x = ref_airy_x[i];
        INFO("x=" << x);
        CHECK(close(sf::airy_ai(x), ref_airy_ai[i], 1e-11, 1e-13));
        CHECK(close(sf::airy_ai_prime(x), ref_airy_aip[i], 1e-11, 1e-13));
        CHECK(close(sf::airy_bi(x), ref_airy_bi[i], 1e-11, 1e-13));
        CHECK(close(sf::airy_bi_prime(x), ref_airy_bip[i], 1e-11, 1e-13));
    }
}

TEST_CASE("bessel: Airy below the Maclaurin range vs scipy", "[v12-b][special][airy]")
{
    // For x < 0 the series terms reach e^ζ with ζ = (2/3)|x|^{3/2}; between −9 and −5.5 it was off by up to 2.6e3 times
    // the gate, so those points go through the Bessel branch. Beyond x = −600 the continued fraction of that branch
    // needs about ζ steps and ran out (x = −800 was off by O(1)): from ζ = 1000 on Hankel's expansion serves.
    for (std::size_t i = 0; i < len(ref_airy2_x); ++i)
    {
        const f64 x = ref_airy2_x[i];
        INFO("x=" << x);
        CHECK(close(sf::airy_ai(x), ref_airy2_ai[i], 1e-11, 1e-13));
        CHECK(close(sf::airy_ai_prime(x), ref_airy2_aip[i], 1e-11, 1e-13));
        CHECK(close(sf::airy_bi(x), ref_airy2_bi[i], 1e-11, 1e-13));
        CHECK(close(sf::airy_bi_prime(x), ref_airy2_bip[i], 1e-11, 1e-13));
    }
}

TEST_CASE("bessel: Kelvin ber/bei/ker/kei vs scipy", "[v12-b][special][kelvin]")
{
    // ber/bei (which grow) hold ~1e-11. ker/kei are exponentially small while the ascending-series terms are
    // exponentially large, so the crossover (x~6–12) is genuinely double-precision-limited to ~1e-8 (asymptotic is
    // no better there); small x and the large-x asymptotic path (x≥18) are ~1e-12. The 1e-8 rtol reflects that real
    // conditioning limit honestly — not a loose gate.
    for (std::size_t i = 0; i < len(ref_kelvin_x); ++i)
    {
        const f64 x = ref_kelvin_x[i];
        INFO("x=" << x);
        CHECK(close(sf::kelvin_ber(x), ref_kelvin_ber[i], 1e-10, 1e-12));
        CHECK(close(sf::kelvin_bei(x), ref_kelvin_bei[i], 1e-10, 1e-12));
        CHECK(close(sf::kelvin_ker(x), ref_kelvin_ker[i], 1e-8, 1e-12));
        CHECK(close(sf::kelvin_kei(x), ref_kelvin_kei[i], 1e-8, 1e-12));
    }
}

TEST_CASE("bessel: Kelvin ker/kei in the crossover vs scipy", "[v12-b][special][kelvin]")
{
    // The ascending series of ker/kei loses e^{√2 x}·ε (3e-7 at x = 11, 1e-5 at x = 18), so the asymptotic takes over
    // from x = 9; the bar is the one of the test above.
    for (std::size_t i = 0; i < len(ref_kelvin2_x); ++i)
    {
        const f64 x = ref_kelvin2_x[i];
        INFO("x=" << x);
        CHECK(close(sf::kelvin_ker(x), ref_kelvin2_ker[i], 1e-8, 1e-12));
        CHECK(close(sf::kelvin_kei(x), ref_kelvin2_kei[i], 1e-8, 1e-12));
    }
}

// Independent identity checks (no external ref): the Wronskians pin the relative normalisation of the pairs.
TEST_CASE("bessel: Wronskian identities", "[v12-b][special][bessel]")
{
    for (const f64 nu : {-5.0, -2.3, -1.5, -0.5, 0.0, 0.5, 1.0, 2.3, 5.0})
    {
        for (const f64 x : {0.3, 1.0, 3.0, 7.0, 15.0})
        {
            INFO("nu=" << nu << " x=" << x);
            // J Y' − J' Y = 2/(πx)
            const f64 wjy = sf::cyl_bessel_j(nu, x) * sf::cyl_neumann_prime(nu, x) -
                            sf::cyl_bessel_j_prime(nu, x) * sf::cyl_neumann(nu, x);
            CHECK(close(wjy, 2.0 / (3.14159265358979323846 * x), 1e-11, 1e-13));
            // I K' − I' K = −1/x
            const f64 wik = sf::cyl_bessel_i(nu, x) * sf::cyl_bessel_k_prime(nu, x) -
                            sf::cyl_bessel_i_prime(nu, x) * sf::cyl_bessel_k(nu, x);
            CHECK(close(wik, -1.0 / x, 1e-11, 1e-13));
        }
    }
}

TEST_CASE("bessel: complex argument J/Y/I/K vs scipy", "[v12-b][special][bessel][complex]")
{
    for (std::size_t i = 0; i < len(ref_cbesj_re); ++i)
    {
        const f64 nu = ref_cbes_nu[i];
        const std::complex<f64> z(ref_cbes_zr[i], ref_cbes_zi[i]);
        INFO("nu=" << nu << " z=(" << ref_cbes_zr[i] << "," << ref_cbes_zi[i] << ")");
        const auto j = sf::cyl_bessel_j(nu, z);
        CHECK(close(j.real(), ref_cbesj_re[i], 1e-10, 1e-11));
        CHECK(close(j.imag(), ref_cbesj_im[i], 1e-10, 1e-11));
        const auto iv = sf::cyl_bessel_i(nu, z);
        CHECK(close(iv.real(), ref_cbesi_re[i], 1e-10, 1e-11));
        CHECK(close(iv.imag(), ref_cbesi_im[i], 1e-10, 1e-11));
        // Y/K: non-integer ν via connection, integer ν via the exact DLMF series, large |z| via asymptotic — all ~1e-9.
        const auto y = sf::cyl_neumann(nu, z);
        CHECK(close(y.real(), ref_cbesy_re[i], 1e-9, 1e-11));
        CHECK(close(y.imag(), ref_cbesy_im[i], 1e-9, 1e-11));
        const auto k = sf::cyl_bessel_k(nu, z);
        CHECK(close(k.real(), ref_cbesk_re[i], 1e-9, 1e-11));
        CHECK(close(k.imag(), ref_cbesk_im[i], 1e-9, 1e-11));
    }
}

TEST_CASE("bessel: complex argument in the left half-plane and near the imaginary axis vs scipy",
          "[v12-b][special][bessel][complex]")
{
    // |z| ≥ 17.5, where the expansions are used: J and Y through z → −z (the expansion of J_0 is off by O(1) near the
    // negative real axis), and I with its second exponential (the first one alone is wrong on and left of the
    // imaginary axis).
    for (std::size_t i = 0; i < len(ref_cbes2j_re); ++i)
    {
        const f64 nu = ref_cbes2_nu[i];
        const std::complex<f64> z(ref_cbes2_zr[i], ref_cbes2_zi[i]);
        INFO("nu=" << nu << " z=(" << ref_cbes2_zr[i] << "," << ref_cbes2_zi[i] << ")");
        const auto j = sf::cyl_bessel_j(nu, z);
        CHECK(close(j.real(), ref_cbes2j_re[i], 1e-10, 1e-11));
        CHECK(close(j.imag(), ref_cbes2j_im[i], 1e-10, 1e-11));
        const auto y = sf::cyl_neumann(nu, z);
        CHECK(close(y.real(), ref_cbes2y_re[i], 1e-9, 1e-11));
        CHECK(close(y.imag(), ref_cbes2y_im[i], 1e-9, 1e-11));
        const auto iv = sf::cyl_bessel_i(nu, z);
        CHECK(close(iv.real(), ref_cbes2i_re[i], 1e-10, 1e-11));
        CHECK(close(iv.imag(), ref_cbes2i_im[i], 1e-10, 1e-11));
        const auto k = sf::cyl_bessel_k(nu, z);
        CHECK(close(k.real(), ref_cbes2k_re[i], 1e-9, 1e-11));
        CHECK(close(k.imag(), ref_cbes2k_im[i], 1e-9, 1e-11));
    }
}

TEST_CASE("bessel: complex argument with the order near |z| vs scipy", "[v12-b][special][bessel][complex]")
{
    // Both the series and Hankel's expansion are about 1e-7 there (J_50(60+10i) was 3.4e-7 off): Hankel's expansion at
    // the base order with the recurrence in order serves J and Y, and Steed's CF2 with the recurrence serves K between
    // |z| = 8 and the rounding of its asymptotic (K_5(9+0.5i) was 1.6e-7 off).
    for (std::size_t i = 0; i < len(ref_cbes3j_re); ++i)
    {
        const f64 nu = ref_cbes3_nu[i];
        const std::complex<f64> z(ref_cbes3_zr[i], ref_cbes3_zi[i]);
        INFO("nu=" << nu << " z=(" << ref_cbes3_zr[i] << "," << ref_cbes3_zi[i] << ")");
        const auto j = sf::cyl_bessel_j(nu, z);
        CHECK(close(j.real(), ref_cbes3j_re[i], 1e-10, 1e-11));
        CHECK(close(j.imag(), ref_cbes3j_im[i], 1e-10, 1e-11));
        const auto y = sf::cyl_neumann(nu, z);
        CHECK(close(y.real(), ref_cbes3y_re[i], 1e-9, 1e-11));
        CHECK(close(y.imag(), ref_cbes3y_im[i], 1e-9, 1e-11));
    }
    for (std::size_t i = 0; i < len(ref_cbes3k_re); ++i)
    {
        const f64 nu = ref_cbes3k_nu[i];
        const std::complex<f64> z(ref_cbes3k_zr[i], ref_cbes3k_zi[i]);
        INFO("nu=" << nu << " z=(" << ref_cbes3k_zr[i] << "," << ref_cbes3k_zi[i] << ")");
        const auto k = sf::cyl_bessel_k(nu, z);
        CHECK(close(k.real(), ref_cbes3k_re[i], 1e-9, 1e-11));
        CHECK(close(k.imag(), ref_cbes3k_im[i], 1e-9, 1e-11));
    }
}

TEST_CASE("bessel: K, Y, I, J and their slopes at the smallest subnormal arguments", "[v12-b][special][bessel]")
{
    // 0.5·x rounds to 0 for x = 4.94e-324 and its logarithm is −∞: Temme's head (K, Y) and the ascending series (I, J
    // and their slopes) turned into NaN there. A result beyond the doubles must come back as an infinity.
    for (std::size_t i = 0; i < len(ref_bessx_j); ++i)
    {
        const f64 nu = ref_bessx_nu[i];
        const f64 x = ref_bessx_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(tiny_close(sf::cyl_bessel_j(nu, x), ref_bessx_j[i]));
        CHECK(tiny_close(sf::cyl_neumann(nu, x), ref_bessx_y[i]));
        CHECK(tiny_close(sf::cyl_bessel_i(nu, x), ref_bessx_i[i]));
        CHECK(tiny_close(sf::cyl_bessel_k(nu, x), ref_bessx_k[i]));
        CHECK(tiny_close(sf::cyl_bessel_j_prime(nu, x), ref_bessx_jp[i]));
        CHECK(tiny_close(sf::cyl_neumann_prime(nu, x), ref_bessx_yp[i]));
        CHECK(tiny_close(sf::cyl_bessel_i_prime(nu, x), ref_bessx_ip[i]));
        CHECK(tiny_close(sf::cyl_bessel_k_prime(nu, x), ref_bessx_kp[i]));
    }
}

TEST_CASE("bessel: J, Y and their slopes at order above 2e5 and argument above the order vs Debye in mpmath",
          "[v12-b][special][bessel]")
{
    // The recurrence from the base order is capped at 2e5 steps and the continued fraction at 1e4 terms: above that
    // order Debye's expansion at a margin below x (phase in double-double arithmetic) and the recurrence in order
    // serve, and from x = 1e15 Hankel's expansion at the order itself. The references are Debye's expansion of 28 terms
    // in mpmath.
    for (std::size_t i = 0; i < len(ref_besl2_j); ++i)
    {
        const f64 nu = ref_besl2_nu[i];
        const f64 x = ref_besl2_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_j(nu, x), ref_besl2_j[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_neumann(nu, x), ref_besl2_y[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_bessel_j_prime(nu, x), ref_besl2_jp[i], 1e-12, 1e-13));
        CHECK(close(sf::cyl_neumann_prime(nu, x), ref_besl2_yp[i], 1e-12, 1e-13));
    }
}

TEST_CASE("bessel: Wronskian at order above 2e5", "[v12-b][special][bessel]")
{
    // J Y' − J' Y = 2/(πx) holds whatever the phase, so it checks the four results against each other.
    for (std::size_t i = 0; i < len(ref_besl2_j); ++i)
    {
        const f64 nu = ref_besl2_nu[i];
        const f64 x = ref_besl2_x[i];
        INFO("nu=" << nu << " x=" << x);
        const f64 w = sf::cyl_bessel_j(nu, x) * sf::cyl_neumann_prime(nu, x) -
                      sf::cyl_bessel_j_prime(nu, x) * sf::cyl_neumann(nu, x);
        CHECK(close(w, 2.0 / (3.14159265358979323846 * x), 1e-9, 0.0));
    }
}

TEST_CASE("bessel: complex J and Y with the order above 3/4 Re z and Im z of 15 and more vs mpmath",
          "[v12-b][special][bessel][complex]")
{
    // Hankel's expansion at the base order with the recurrence in order amplifies its start by e^{2(Im z − Im φ)}
    // there (1e5 at J_35.2(45+20i), 1e11 at J_50(60+30i), 1e-5 off): J comes from the downward recurrence of its
    // continued fraction, normalised by the Wronskian with H^(1) (K at the rotated argument).
    for (std::size_t i = 0; i < len(ref_cbes4j_re); ++i)
    {
        const f64 nu = ref_cbes4_nu[i];
        const std::complex<f64> z(ref_cbes4_zr[i], ref_cbes4_zi[i]);
        INFO("nu=" << nu << " z=(" << ref_cbes4_zr[i] << "," << ref_cbes4_zi[i] << ")");
        CHECK(close_c(sf::cyl_bessel_j(nu, z), {ref_cbes4j_re[i], ref_cbes4j_im[i]}, 1e-10, 1e-11));
        CHECK(close_c(sf::cyl_neumann(nu, z), {ref_cbes4y_re[i], ref_cbes4y_im[i]}, 1e-9, 1e-11));
    }
}

TEST_CASE("bessel: complex J at order 35.2 and Im z 5 for Re z 31 to 44 vs mpmath", "[v12-b][special][bessel][complex]")
{
    // The ascending series loses more than the bar of the module from Re z = 33.5 on (1e-9 at 36, 1e-6 at 42): from
    // there J comes from Hankel's expansion at the base order with the recurrence in order, or from the expansion at
    // the order itself where that is within the bar.
    for (std::size_t i = 0; i < len(ref_cbes6j_re); ++i)
    {
        const f64 nu = ref_cbes6_nu[i];
        const std::complex<f64> z(ref_cbes6_zr[i], ref_cbes6_zi[i]);
        INFO("nu=" << nu << " z=(" << ref_cbes6_zr[i] << "," << ref_cbes6_zi[i] << ")");
        CHECK(close_c(sf::cyl_bessel_j(nu, z), {ref_cbes6j_re[i], ref_cbes6j_im[i]}, 1e-10, 1e-11));
    }
}

TEST_CASE("bessel: J at -0 is 0 for an order above 0 and stays NaN for a negative argument", "[v12-b][special][bessel]")
{
    for (const f64 nu : {0.5, 1.0, 2.5, 7.3})
    {
        INFO("nu=" << nu);
        CHECK(sf::cyl_bessel_j(nu, -0.0) == 0.0);
        CHECK(sf::cyl_bessel_j(nu, 0.0) == 0.0);
        CHECK(std::isnan(sf::cyl_bessel_j(nu, -1e-300)));
        CHECK(std::isnan(sf::cyl_bessel_j(nu, -1.0)));
    }
}

TEST_CASE("bessel: complex Y at a negative integer order in the left half-plane vs mpmath",
          "[v12-b][special][bessel][complex]")
{
    // The reflection there needs J at the same negative integer order, whose series divides by nu + k = 0; the integer
    // series of base serves.
    for (std::size_t i = 0; i < len(ref_cbes7y_re); ++i)
    {
        const f64 nu = ref_cbes7_nu[i];
        const std::complex<f64> z(ref_cbes7_zr[i], ref_cbes7_zi[i]);
        INFO("nu=" << nu << " z=(" << ref_cbes7_zr[i] << "," << ref_cbes7_zi[i] << ")");
        CHECK(close_c(sf::cyl_neumann(nu, z), {ref_cbes7y_re[i], ref_cbes7y_im[i]}, 1e-9, 1e-11));
    }
}

TEST_CASE("bessel: complex K at a half-integer order in the left half-plane vs mpmath",
          "[v12-b][special][bessel][complex]")
{
    // K comes from I at the mirrored point, whose ascending series carries the bar of K here: I comes from the stable
    // method for J at the rotated argument.
    for (std::size_t i = 0; i < len(ref_cbes8k_re); ++i)
    {
        const f64 nu = ref_cbes8_nu[i];
        const std::complex<f64> z(ref_cbes8_zr[i], ref_cbes8_zi[i]);
        INFO("nu=" << nu << " z=(" << ref_cbes8_zr[i] << "," << ref_cbes8_zi[i] << ")");
        CHECK(close_c(sf::cyl_bessel_k(nu, z), {ref_cbes8k_re[i], ref_cbes8k_im[i]}, 1e-9, 1e-11));
    }
}

TEST_CASE("bessel: I and I' at an order above x, where the core's recurrence leaves the doubles, vs mpmath",
          "[v12-b][special][bessel]")
{
    // Finite values: the class of a NaN must not be decided by e^{x - nu^2/2x}, which holds only for nu << x.
    for (std::size_t i = 0; i < len(ref_besc1_i); ++i)
    {
        const f64 nu = ref_besc1_nu[i];
        const f64 x = ref_besc1_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_i(nu, x), ref_besc1_i[i], 1e-12, 1e-300));
        CHECK(close(sf::cyl_bessel_i_prime(nu, x), ref_besc1_ip[i], 1e-12, 1e-300));
    }
}

TEST_CASE("bessel: complex Y within 1e-12 of a negative integer in the left half-plane vs mpmath",
          "[v12-b][special][bessel][complex]")
{
    // The reflection there reaches the series of J at nu + k of about 1e-14: base's integer series serves.
    for (std::size_t i = 0; i < len(ref_cbes13y_re); ++i)
    {
        const f64 nu = ref_cbes13_nu[i];
        const std::complex<f64> z(ref_cbes13_zr[i], ref_cbes13_zi[i]);
        INFO("nu=" << nu << " z=(" << ref_cbes13_zr[i] << "," << ref_cbes13_zi[i] << ")");
        CHECK(close_c(sf::cyl_neumann(nu, z), {ref_cbes13y_re[i], ref_cbes13y_im[i]}, 1e-9, 1e-11));
    }
}

TEST_CASE("bessel: I and I' of a negative order at x from 745 reflect through the scale-carrying K",
          "[v12-b][special][bessel]")
{
    // I_{-nu} = I_nu + (2/pi) sin(nu pi) K_nu: K carries the value here; base was NaN or an infinity (the reference is
    // formed from the positive-order functions at 200 digits, in the generator).
    const f64 inf = std::numeric_limits<f64>::infinity();
    CHECK(close(sf::cyl_bessel_i(-2443.33, 1387.66), ref_besd2_i, 1e-12, 1e-300));
    // K starts from a subnormal e^{-x} at the plain route; 1.3e-7 off before K carried its scale.
    CHECK(close(sf::cyl_bessel_i(-1591.8123, 725.5648), -1.0256393708935943e294, 1e-12, 1e-300));
    CHECK(sf::cyl_bessel_i(-4927.74, 1460.52) == -inf);    // truth -1.8e1897
    CHECK(sf::cyl_bessel_i(-1791.4838, 744.7005) == -inf); // K of 5.7e409 carries it
    CHECK(sf::cyl_bessel_i_prime(-1791.4838, 744.7005) == inf);
    CHECK(sf::cyl_bessel_i_prime(-3110.12, 798.33) == -inf);
    // K overflows before the coefficient of the reflection brings it back into the doubles: the scale is applied last.
    CHECK(close(sf::cyl_bessel_i(-2116.5429919096973, 1047.270635449522), 1.7102410250884557e308, 1e-12, 1e-300));
}

TEST_CASE("bessel: I and I' are finite at a large argument and an order near it vs mpmath", "[v12-b][special][bessel]")
{
    // e^{-x} of CF2 is below the doubles from x = 745 and I_mu's factor e^{x} above them from 713: the scale is carried
    // through the Wronskian.
    for (std::size_t i = 0; i < len(ref_besc2_i); ++i)
    {
        const f64 nu = ref_besc2_nu[i];
        const f64 x = ref_besc2_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_i(nu, x), ref_besc2_i[i], 1e-12, 1e-300));
        CHECK(close(sf::cyl_bessel_i_prime(nu, x), ref_besc2_ip[i], 1e-12, 1e-300));
    }
    CHECK(sf::cyl_bessel_i(0.0, 720.0) == std::numeric_limits<f64>::infinity());
}

TEST_CASE("bessel: K and K' keep their scale at x from 745, and leave the doubles only where they do",
          "[v12-b][special][bessel]")
{
    for (std::size_t i = 0; i < len(ref_besc3_k); ++i)
    {
        const f64 nu = ref_besc3_nu[i];
        const f64 x = ref_besc3_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_k(nu, x), ref_besc3_k[i], 1e-12, 1e-300));
        CHECK(close(sf::cyl_bessel_k_prime(nu, x), ref_besc3_kp[i], 1e-12, 1e-300));
    }
    const f64 inf = std::numeric_limits<f64>::infinity();
    CHECK(sf::cyl_bessel_k(3000.0, 745.0) == inf); // truth 3.3e1393
    CHECK(sf::cyl_bessel_k_prime(3000.0, 745.0) == -inf);
    CHECK(sf::cyl_bessel_k(10.0, 800.0) == 0.0); // e^{-800} underflows
}

TEST_CASE("bessel: J and J' at a large order are 0 where the core's recurrence leaves the doubles, and the reflections "
          "are 0 or infinite",
          "[v12-b][special][bessel]")
{
    const f64 inf = std::numeric_limits<f64>::infinity();
    CHECK(sf::cyl_bessel_j(1234.0670925846282, 12.001933414083446) == 0.0);
    CHECK(sf::cyl_bessel_j(1403.7387915091808, 378.5275359961738) == 0.0);
    CHECK(sf::cyl_bessel_j(883.5, 46.99142257734836) == 0.0);
    CHECK(sf::cyl_bessel_j_prime(303.0, 0.34) == 0.0);
    CHECK(sf::cyl_bessel_j_prime(1174.0, 0.0144) == 0.0);
    CHECK(sf::cyl_bessel_j(-332.373058014091, 0.12114020729260479) == inf);  // truth 1.3e1097
    CHECK(sf::cyl_bessel_j(-401.56354207846954, 6.217408954219093) == -inf); // truth -9.5e671
    CHECK(sf::cyl_neumann(-1780.5, 0.012022098263197304) == 0.0);            // truth 1.2e-8971
    CHECK(sf::cyl_bessel_j(-177.0, 0.0038136174181590187) == 0.0);           // truth -1.2e-804
    CHECK(std::isinf(sf::cyl_bessel_j_prime(-332.373058014091, 0.12114020729260479)));
    CHECK(std::isinf(sf::cyl_neumann_prime(-332.373058014091, 0.12114020729260479)));
    CHECK(std::isnan(sf::cyl_bessel_j(1234.5, -1.0))); // a negative argument stays a domain error
}

TEST_CASE("bessel: complex J and I at a negative integer order vs mpmath", "[v12-b][special][bessel][complex]")
{
    // The series divides by nu + k = 0 there: J_{-n} = (-1)^n J_n and I_{-n} = I_n.
    for (std::size_t i = 0; i < len(ref_cbes9j_re); ++i)
    {
        const std::complex<f64> z(ref_cbes9_zr[i], ref_cbes9_zi[i]);
        INFO("nu=" << ref_cbes9_nu[i] << " z=(" << z.real() << "," << z.imag() << ")");
        CHECK(close_c(sf::cyl_bessel_j(ref_cbes9_nu[i], z), {ref_cbes9j_re[i], ref_cbes9j_im[i]}, 1e-10, 1e-11));
    }
    for (std::size_t i = 0; i < len(ref_cbes10i_re); ++i)
    {
        const std::complex<f64> z(ref_cbes10_zr[i], ref_cbes10_zi[i]);
        INFO("nu=" << ref_cbes10_nu[i] << " z=(" << z.real() << "," << z.imag() << ")");
        CHECK(close_c(sf::cyl_bessel_i(ref_cbes10_nu[i], z), {ref_cbes10i_re[i], ref_cbes10i_im[i]}, 1e-10, 1e-11));
    }
}

TEST_CASE("bessel: complex Y at a negative half-integer order vs mpmath", "[v12-b][special][bessel][complex]")
{
    // cos(nu pi) is exactly zero there; its residue of 1e-16 would multiply the huge J at that order.
    for (std::size_t i = 0; i < len(ref_cbes11y_re); ++i)
    {
        const std::complex<f64> z(ref_cbes11_zr[i], ref_cbes11_zi[i]);
        INFO("nu=" << ref_cbes11_nu[i] << " z=(" << z.real() << "," << z.imag() << ")");
        CHECK(close_c(sf::cyl_neumann(ref_cbes11_nu[i], z), {ref_cbes11y_re[i], ref_cbes11y_im[i]}, 1e-9, 1e-11));
    }
}

TEST_CASE("bessel: complex I and K at a large imaginary part and an order about |z| vs mpmath",
          "[v12-b][special][bessel][complex]")
{
    // The series of I loses more than the bar there: I comes from the stable method for J at the rotated argument, and
    // K of the left half-plane through it.
    for (std::size_t i = 0; i < len(ref_cbes12v_re); ++i)
    {
        const std::complex<f64> z(ref_cbes12_zr[i], ref_cbes12_zi[i]);
        const std::complex<f64> ref(ref_cbes12v_re[i], ref_cbes12v_im[i]);
        INFO("nu=" << ref_cbes12_nu[i] << " z=(" << z.real() << "," << z.imag() << ")");
        const bool is_k = ref_cbes12_kind[i] == 1;
        const std::complex<f64> got =
            is_k ? sf::cyl_bessel_k(ref_cbes12_nu[i], z) : sf::cyl_bessel_i(ref_cbes12_nu[i], z);
        CHECK(close_c(got, ref, is_k ? 1e-9 : 1e-10, 1e-11));
    }
}

TEST_CASE("bessel: K, Y, I, J in the first normal binade, where 0.5*x is a subnormal", "[v12-b][special][bessel]")
{
    // x in [2^-1022, 2^-1021): ln(x/2) comes from the integer count of x, as it does below the smallest normal number.
    for (std::size_t i = 0; i < len(ref_bessy_k); ++i)
    {
        const f64 nu = ref_bessy_nu[i];
        const f64 x = ref_bessy_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(tiny_close(sf::cyl_bessel_k(nu, x), ref_bessy_k[i]));
        CHECK(tiny_close(sf::cyl_neumann(nu, x), ref_bessy_y[i]));
        CHECK(tiny_close(sf::cyl_bessel_i(nu, x), ref_bessy_i[i]));
        CHECK(tiny_close(sf::cyl_bessel_j(nu, x), ref_bessy_j[i]));
    }
}

TEST_CASE("bessel: J, Y and their slopes at order from 2500 (to 1e75) and argument from 1e6 vs Hankel in mpmath",
          "[v12-b][special][bessel]")
{
    // Hankel's expansion at the order itself carries these (nu^2 <= 12 x): from nu = 2000 its coefficients and powers
    // of 1/x leave the doubles separately (the sums are carried as one product), from 2^52 its phase needs the 1/4 of
    // nu/2 + 1/4 after the reduction of nu/2, and the slopes need the order nu + 1, which is no other double from 2^53
    // on.
    for (std::size_t i = 0; i < len(ref_besw_j); ++i)
    {
        const f64 nu = ref_besw_nu[i];
        const f64 x = ref_besw_x[i];
        INFO("nu=" << nu << " x=" << x);
        CHECK(close(sf::cyl_bessel_j(nu, x), ref_besw_j[i], 1e-10, 0.0));
        CHECK(close(sf::cyl_neumann(nu, x), ref_besw_y[i], 1e-10, 0.0));
        CHECK(close(sf::cyl_bessel_j_prime(nu, x), ref_besw_jp[i], 1e-10, 0.0));
        CHECK(close(sf::cyl_neumann_prime(nu, x), ref_besw_yp[i], 1e-10, 0.0));
    }
}

TEST_CASE("bessel: J, Y and their slopes are NaN where no method of the module serves", "[v12-b][special][bessel]")
{
    // From x = 1e20 the phase of Debye's expansion in double-double arithmetic (1e-32 x absolute) is above the bar of
    // the module, and Hankel's expansion at the order is not reliable for nu^2 > 12 x: the answer is NaN (not the Steed
    // fraction's value, and not an infinity from the overflow class of Y).
    const f64 points[][2] = {{3e11, 5e21},   {1e12, 1e22},   {1e20, 1e30},  {1e80, 1e150},
                             {1e140, 1e150}, {1e110, 1e200}, {1e151, 1e300}};
    for (const auto& p : points)
    {
        INFO("nu=" << p[0] << " x=" << p[1]);
        CHECK(std::isnan(sf::cyl_bessel_j(p[0], p[1])));
        CHECK(std::isnan(sf::cyl_neumann(p[0], p[1])));
        CHECK(std::isnan(sf::cyl_bessel_j_prime(p[0], p[1])));
        CHECK(std::isnan(sf::cyl_neumann_prime(p[0], p[1])));
    }
}

TEST_CASE("bessel: complex J and Y at an imaginary part of 600 and more vs mpmath", "[v12-b][special][bessel][complex]")
{
    // The ascending series cannot be summed in 400 terms there (it was chosen for its estimate all the same), the
    // series of Y through J_{-nu} alternates, and e^{Im z} leaves the doubles in the cosine before it does in J.
    for (std::size_t i = 0; i < len(ref_cbes5j_re); ++i)
    {
        const f64 nu = ref_cbes5_nu[i];
        const std::complex<f64> z(ref_cbes5_zr[i], ref_cbes5_zi[i]);
        INFO("nu=" << nu << " z=(" << ref_cbes5_zr[i] << "," << ref_cbes5_zi[i] << ")");
        CHECK(close_c(sf::cyl_bessel_j(nu, z), {ref_cbes5j_re[i], ref_cbes5j_im[i]}, 1e-10, 1e-11));
        CHECK(close_c(sf::cyl_neumann(nu, z), {ref_cbes5y_re[i], ref_cbes5y_im[i]}, 1e-9, 1e-11));
    }
}

TEST_CASE("bessel: complex J and Y of order 2e5 and more are NaN where no method serves",
          "[v12-b][special][bessel][complex]")
{
    // The recurrences stop at 2e5 steps and the series and Hankel's expansion have no accuracy for z near the order:
    // the answer is NaN, not a number from a series whose terms left the doubles.
    const f64 orders[] = {200000.5, 300000.25, 1000000.3};
    const f64 factors[] = {0.5, 1.0, 1.2, 3.0};
    for (f64 nu : orders)
    {
        for (f64 factor : factors)
        {
            const std::complex<f64> z(factor * nu, 10.0);
            INFO("nu=" << nu << " z=(" << z.real() << "," << z.imag() << ")");
            const std::complex<f64> j = sf::cyl_bessel_j(nu, z);
            const std::complex<f64> y = sf::cyl_neumann(nu, z);
            CHECK((std::isnan(j.real()) || std::isnan(j.imag())));
            CHECK((std::isnan(y.real()) || std::isnan(y.imag())));
        }
    }
}

TEST_CASE("bessel: f32 sanity", "[v12-b][special][bessel]")
{
    CHECK(close(static_cast<f64>(sf::cyl_bessel_j(0.0F, 1.0F)), 0.7651976865579666, 1e-6, 1e-7));
    CHECK(close(static_cast<f64>(sf::airy_ai(1.0F)), 0.1352924163128814, 1e-6, 1e-7));
    CHECK(close(static_cast<f64>(sf::cyl_bessel_k(1.0F, 2.0F)), 0.1398658818165224, 1e-6, 1e-7));
}
