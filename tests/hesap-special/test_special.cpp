// crd-hesap-special v12-a — gamma / incomplete / erf gates. Reference vectors GENERATED from scipy.special
// (special_refs.inc, plain C arrays) + an independent std:: cross-check (lgamma/tgamma/erf/erfc) + run-twice
// determinism + an f32 accuracy probe. Gate: f64 ≤ 1e-12 (closed-form) / ≤ 1e-9 (Halley inverses).

#include <crd/hesap/special/special.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace sf = crd::hesap::special;
using crd::f64;

namespace
{
#include "special_refs.inc"

void chk(f64 got, f64 expected, f64 rtol, f64 atol, const char* tag, std::size_t i)
{
    INFO(tag << "[" << i << "] got=" << got << " expected=" << expected);
    CHECK(std::abs(got - expected) <= atol + rtol * std::abs(expected));
}

template <std::size_t N>
constexpr std::size_t len(const double (&)[N]) noexcept
{
    return N;
}
} // namespace

TEST_CASE("special gamma: lgamma/tgamma/digamma/trigamma vs scipy", "[v12-a][special][gamma]")
{
    for (std::size_t i = 0; i < len(ref_gamma_x); ++i)
    {
        chk(sf::lgamma(ref_gamma_x[i]), ref_lgamma_y[i], 1e-13, 1e-13, "lgamma", i);
        chk(sf::gamma(ref_gamma_x[i]), ref_tgamma_y[i], 1e-12, 0.0, "tgamma", i);
    }
    for (std::size_t i = 0; i < len(ref_digamma_x); ++i)
    {
        chk(sf::digamma(ref_digamma_x[i]), ref_digamma_y[i], 1e-12, 1e-13, "digamma", i);
    }
    for (std::size_t i = 0; i < len(ref_trigamma_x); ++i)
    {
        chk(sf::trigamma(ref_trigamma_x[i]), ref_trigamma_y[i], 1e-12, 1e-13, "trigamma", i);
    }
    for (std::size_t i = 0; i < len(ref_polygamma_x); ++i)
    {
        chk(sf::polygamma(static_cast<int>(ref_polygamma_n[i]), ref_polygamma_x[i]), ref_polygamma_y[i], 1e-9, 1e-11,
            "polygamma", i);
    }
}

TEST_CASE("special beta: beta/lbeta vs scipy", "[v12-a][special][gamma]")
{
    for (std::size_t i = 0; i < len(ref_beta_a); ++i)
    {
        chk(sf::beta(ref_beta_a[i], ref_beta_b[i]), ref_beta_y[i], 1e-12, 0.0, "beta", i);
        chk(sf::lbeta(ref_beta_a[i], ref_beta_b[i]), ref_lbeta_y[i], 1e-13, 1e-13, "lbeta", i);
    }
}

TEST_CASE("special incomplete gamma: P/Q + inverse vs scipy", "[v12-a][special][incomplete]")
{
    for (std::size_t i = 0; i < len(ref_gammainc_a); ++i)
    {
        chk(sf::gammainc_p(ref_gammainc_a[i], ref_gammainc_x[i]), ref_gammainc_p[i], 1e-12, 1e-14, "gammainc_p", i);
        chk(sf::gammainc_q(ref_gammainc_a[i], ref_gammainc_x[i]), ref_gammainc_q[i], 1e-12, 1e-14, "gammainc_q", i);
        // P + Q == 1 identity.
        CHECK(std::abs(sf::gammainc_p(ref_gammainc_a[i], ref_gammainc_x[i]) +
                       sf::gammainc_q(ref_gammainc_a[i], ref_gammainc_x[i]) - 1.0) < 1e-13);
    }
    for (std::size_t i = 0; i < len(ref_gammaincinv_a); ++i)
    {
        chk(sf::gammainc_p_inv(ref_gammaincinv_a[i], ref_gammaincinv_p[i]), ref_gammaincinv_y[i], 1e-9, 1e-11,
            "gammaincinv", i);
        // Round trip P(a, P⁻¹(a,p)) == p.
        const f64 x = sf::gammainc_p_inv(ref_gammaincinv_a[i], ref_gammaincinv_p[i]);
        CHECK(std::abs(sf::gammainc_p(ref_gammaincinv_a[i], x) - ref_gammaincinv_p[i]) < 1e-10);
    }
}

TEST_CASE("special incomplete beta: I_x + inverse vs scipy", "[v12-a][special][incomplete]")
{
    for (std::size_t i = 0; i < len(ref_betainc_a); ++i)
    {
        chk(sf::betainc(ref_betainc_a[i], ref_betainc_b[i], ref_betainc_x[i]), ref_betainc_y[i], 1e-12, 1e-14,
            "betainc", i);
    }
    for (std::size_t i = 0; i < len(ref_betaincinv_a); ++i)
    {
        chk(sf::betainc_inv(ref_betaincinv_a[i], ref_betaincinv_b[i], ref_betaincinv_p[i]), ref_betaincinv_y[i], 1e-9,
            1e-11, "betaincinv", i);
        const f64 x = sf::betainc_inv(ref_betaincinv_a[i], ref_betaincinv_b[i], ref_betaincinv_p[i]);
        CHECK(std::abs(sf::betainc(ref_betaincinv_a[i], ref_betaincinv_b[i], x) - ref_betaincinv_p[i]) < 1e-10);
    }
}

TEST_CASE("special erf: erf/erfc/erfcx/erfinv/erfcinv/dawson vs scipy", "[v12-a][special][erf]")
{
    for (std::size_t i = 0; i < len(ref_erf_x); ++i)
    {
        chk(sf::erf(ref_erf_x[i]), ref_erf_y[i], 1e-13, 1e-15, "erf", i);
        chk(sf::erfc(ref_erf_x[i]), ref_erfc_y[i], 1e-12, 1e-300, "erfc", i);
    }
    for (std::size_t i = 0; i < len(ref_erfcx_x); ++i)
    {
        chk(sf::erfcx(ref_erfcx_x[i]), ref_erfcx_y[i], 1e-12, 1e-14, "erfcx", i);
    }
    for (std::size_t i = 0; i < len(ref_erfinv_y); ++i)
    {
        chk(sf::erfinv(ref_erfinv_y[i]), ref_erfinv_x[i], 1e-10, 1e-12, "erfinv", i);
    }
    for (std::size_t i = 0; i < len(ref_erfcinv_y); ++i)
    {
        chk(sf::erfcinv(ref_erfcinv_y[i]), ref_erfcinv_x[i], 1e-10, 1e-12, "erfcinv", i);
    }
    for (std::size_t i = 0; i < len(ref_dawson_x); ++i)
    {
        chk(sf::dawson(ref_dawson_x[i]), ref_dawson_y[i], 1e-12, 1e-14, "dawson", i);
    }
}

TEST_CASE("special: independent std:: cross-check (oracle without the .inc)", "[v12-a][special]")
{
    for (double x : {0.3, 0.7, 1.0, 1.5, 2.0, 3.5, 7.0, 12.5, 30.0})
    {
        CHECK(std::abs(sf::lgamma(x) - std::lgamma(x)) <= 1e-13 + 1e-13 * std::abs(std::lgamma(x)));
        CHECK(std::abs(sf::gamma(x) - std::tgamma(x)) <= 1e-12 * std::abs(std::tgamma(x)));
    }
    for (double x : {-3.0, -1.2, -0.4, 0.0, 0.4, 1.2, 2.6, 4.0})
    {
        CHECK(std::abs(sf::erf(x) - std::erf(x)) <= 1e-13 + 1e-13 * std::abs(std::erf(x)));
        CHECK(std::abs(sf::erfc(x) - std::erfc(x)) <= 1e-12 * std::abs(std::erfc(x)) + 1e-300);
    }
}

TEST_CASE("special: run-twice bit-identical (determinism moat)", "[v12-a][special][moat]")
{
    for (double x : {0.37, 1.9, 5.5, 12.3})
    {
        CHECK(sf::lgamma(x) == sf::lgamma(x));
        CHECK(sf::digamma(x) == sf::digamma(x));
        CHECK(sf::erf(x) == sf::erf(x));
        CHECK(sf::gammainc_p(2.5, x) == sf::gammainc_p(2.5, x));
        CHECK(sf::gammainc_p_inv(2.5, 0.3) == sf::gammainc_p_inv(2.5, 0.3));
    }
}

TEST_CASE("special: f32 instantiation + accuracy", "[v12-a][special][f32]")
{
    for (float x : {0.5F, 1.5F, 3.0F, 7.0F})
    {
        CHECK(std::abs(sf::lgamma(x) - static_cast<float>(std::lgamma(static_cast<double>(x)))) < 1e-4F);
        CHECK(std::abs(sf::erf(x) - std::erf(x)) < 1e-5F);
    }
    CHECK(std::abs(sf::erfinv(0.5F) - 0.4769362762F) < 1e-4F);
}
