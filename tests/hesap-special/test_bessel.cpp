// crd-hesap-special v12-b — Bessel / Airy / Kelvin gated vs scipy.special + Wronskian self-checks + f32 sanity.

#include <crd/hesap/special/special.hpp>

#include "bessel_refs.inc"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>

namespace sf = crd::hesap::special;
using crd::f64;

namespace
{
bool close(f64 got, f64 ref, f64 rtol, f64 atol) noexcept
{
    return std::abs(got - ref) <= atol + rtol * std::abs(ref);
}
template <class A>
constexpr std::size_t len(const A& a) noexcept
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

// Independent identity checks (no external ref): the Wronskians pin the relative normalisation of the pairs.
TEST_CASE("bessel: Wronskian identities", "[v12-b][special][bessel]")
{
    for (const f64 nu : {0.0, 0.5, 1.0, 2.3, 5.0})
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

TEST_CASE("bessel: f32 sanity", "[v12-b][special][bessel]")
{
    CHECK(close(static_cast<f64>(sf::cyl_bessel_j(0.0F, 1.0F)), 0.7651976865579666, 1e-6, 1e-7));
    CHECK(close(static_cast<f64>(sf::airy_ai(1.0F)), 0.1352924163128814, 1e-6, 1e-7));
    CHECK(close(static_cast<f64>(sf::cyl_bessel_k(1.0F, 2.0F)), 0.1398658818165224, 1e-6, 1e-7));
}
