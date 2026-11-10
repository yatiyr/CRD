// crd-hesap-special v12-a — gamma / incomplete / erf gates. Reference vectors GENERATED from scipy.special
// (special_refs.inc, plain C arrays) + an independent std:: cross-check (lgamma/tgamma/erf/erfc) + run-twice
// determinism + an f32 accuracy probe. Gate: f64 ≤ 1e-12 (closed-form) / ≤ 1e-9 (Halley inverses).

#include <crd/hesap/special/detail/lanczos_core.hpp>
#include <crd/hesap/special/special.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>

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

TEST_CASE("special gamma: the poles, signed zeros, infinities and NaN follow C's lgamma and tgamma",
          "[v12-a][special][gamma]")
{
    const f64 inf = std::numeric_limits<f64>::infinity();
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    // The public generic functions and the out-of-line Lanczos scalars the batch kernels reproduce.
    f64 (*const lgammas[2])(f64) = {&sf::lgamma<f64>, &sf::detail::lanczos_lgamma};
    f64 (*const gammas[2])(f64) = {&sf::gamma<f64>, &sf::detail::lanczos_tgamma};
    for (int f = 0; f < 2; ++f)
    {
        INFO((f == 0 ? "public" : "lanczos"));
        CHECK(lgammas[f](inf) == inf);
        CHECK(lgammas[f](-inf) == inf);
        CHECK(lgammas[f](0.0) == inf);
        CHECK(lgammas[f](-0.0) == inf);
        CHECK(lgammas[f](-3.0) == inf);
        CHECK(std::isnan(lgammas[f](nan)));
        CHECK(lgammas[f](1.0e306) == inf); // ln Gamma overflows past about 2.55e305
        CHECK(gammas[f](inf) == inf);
        CHECK(std::isnan(gammas[f](-inf)));
        CHECK(gammas[f](0.0) == inf);
        CHECK(gammas[f](-0.0) == -inf);
        CHECK(std::isnan(gammas[f](-2.0)));
        CHECK(std::isnan(gammas[f](nan)));
        CHECK(gammas[f](172.0) == inf);
        CHECK(gammas[f](5.0e-324) == inf); // Gamma(x) ~ 1/x overflows for the smallest subnormal
    }
}

TEST_CASE("special gamma: gamma_sign is the sign of Gamma on both sides of every negative integer",
          "[v12-a][special][gamma]")
{
    // Gamma is negative on (-1, 0), (-3, -2), ... and positive on (-2, -1), (-4, -3), ...; C's tgamma is the reference.
    CHECK(sf::gamma_sign(-0.5) == -1.0);
    CHECK(sf::gamma_sign(-1.5) == 1.0);
    CHECK(sf::gamma_sign(-2.5) == -1.0);
    CHECK(sf::gamma_sign(-3.5) == 1.0);
    CHECK(sf::gamma_sign(0.5) == 1.0);
    CHECK(sf::gamma_sign(7.25) == 1.0);
    for (int k = 0; k < 24; ++k)
    {
        for (const f64 offset : {1.0e-9, 0.1, 0.5, 0.9, 1.0 - 1.0e-9})
        {
            const f64 x = -static_cast<f64>(k) - offset;
            INFO("x=" << x);
            CHECK(sf::gamma_sign(x) == ((std::tgamma(x) < 0.0) ? -1.0 : 1.0));
            if (offset > 0.01 && offset < 0.99)
            {
                const float xf = static_cast<float>(x);
                CHECK(sf::gamma_sign(xf) == ((std::tgamma(xf) < 0.0F) ? -1.0F : 1.0F));
            }
        }
    }
}

TEST_CASE("special gamma: gamma_sign over many intervals, at the poles and at the specials, in both precisions",
          "[v12-a][special][gamma]")
{
    const auto expected = [](long long k)
    {
        // x in (-(k + 1), -k): the floor is -(k + 1), Gamma is negative exactly when that floor is odd, k even
        return (k % 2) == 0 ? -1.0 : 1.0;
    };
    // every interval up to 4096 and then far out, f64 up to 2^51 and f32 up to 2^22 (both still carry the half)
    for (long long k = 0; k < (1LL << 51); k = k < 4096 ? k + 1 : (k * 3) + 1)
    {
        const f64 x = -static_cast<f64>(k) - 0.5;
        INFO("x=" << x);
        CHECK(sf::gamma_sign(x) == expected(k));
        if (k < (1LL << 22))
        {
            const float xf = -static_cast<float>(k) - 0.5F;
            CHECK(sf::gamma_sign(xf) == static_cast<float>(expected(k)));
        }
        if (k < 160)
        {
            // and against C's tgamma, finite or a signed zero there, on both sides of each interval's centre
            for (const f64 offset : {1.0e-6, 0.25, 0.75, 1.0 - 1.0e-6})
            {
                const f64 y = -static_cast<f64>(k) - offset;
                CHECK(sf::gamma_sign(y) == (std::signbit(std::tgamma(y)) ? -1.0 : 1.0));
            }
        }
    }
    // the poles take the sign of gamma()'s value there: +1 at +0, -1 at -0, NaN at the negative integers and -inf
    const f64 inf = std::numeric_limits<f64>::infinity();
    CHECK(sf::gamma_sign(0.0) == 1.0);
    CHECK(sf::gamma_sign(-0.0) == -1.0);
    CHECK(sf::gamma_sign(0.0F) == 1.0F);
    CHECK(sf::gamma_sign(-0.0F) == -1.0F);
    CHECK(sf::gamma_sign(inf) == 1.0);
    CHECK(std::isnan(sf::gamma_sign(-inf)));
    CHECK(std::isnan(sf::gamma_sign(std::numeric_limits<f64>::quiet_NaN())));
    CHECK(std::isnan(sf::gamma_sign(std::numeric_limits<float>::quiet_NaN())));
    CHECK(std::isnan(sf::gamma_sign(-std::numeric_limits<float>::infinity())));
    for (const f64 pole :
         {-1.0, -2.0, -3.0, -170.0, -171.0, -0x1p52, -0x1p63, -1.0e300, -std::numeric_limits<f64>::max()})
    {
        INFO("pole " << pole);
        CHECK(std::isnan(sf::gamma_sign(pole)));
        CHECK(std::isnan(sf::gamma(pole)));
    }
    for (const float pole : {-1.0F, -2.0F, -0x1p23F, -0x1p40F, -std::numeric_limits<float>::max()})
    {
        INFO("f32 pole " << pole);
        CHECK(std::isnan(sf::gamma_sign(pole)));
    }
    // gamma_sign(x) * exp(lgamma(x)) carries gamma(x)'s sign at the specials and between the poles
    for (const f64 x : {0.0, -0.0, -0.5, -1.5, -2.5, -10.25, -101.75, 0.5, 3.0, 170.5})
    {
        INFO("x=" << x);
        CHECK(std::signbit(sf::gamma_sign(x)) == std::signbit(sf::gamma(x)));
    }
    // the neighbours of a pole read their own intervals, out to the last non-integer doubles and floats
    CHECK(sf::gamma_sign(std::nextafter(-1.0, 0.0)) == -1.0);
    CHECK(sf::gamma_sign(std::nextafter(-1.0, -2.0)) == 1.0);
    CHECK(sf::gamma_sign(-0x1p52 + 0.5) == 1.0);      // in (-2^52, -2^52 + 1): the floor -2^52 is even
    CHECK(std::isnan(sf::gamma_sign(-0x1p52 - 0.5))); // not a double: the sum rounds to the pole -2^52
    CHECK(sf::gamma_sign(-0x1p23F + 0.5F) == 1.0F);
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

TEST_CASE("special incomplete gamma: shapes up to 1e6 vs scipy", "[v12-a][special][incomplete]")
{
    // The series and the fraction run about 8.5*sqrt(a) terms near x = a, and the prefactor x^a e^-x / Gamma(a)
    // cancels three large terms; both used to leave errors of 1e-10 here.
    for (std::size_t i = 0; i < len(ref_gammainc_big_a); ++i)
    {
        chk(sf::gammainc_p(ref_gammainc_big_a[i], ref_gammainc_big_x[i]), ref_gammainc_big_p[i], 1e-12, 1e-14,
            "gammainc_p(big)", i);
        chk(sf::gammainc_q(ref_gammainc_big_a[i], ref_gammainc_big_x[i]), ref_gammainc_big_q[i], 1e-12, 1e-14,
            "gammainc_q(big)", i);
    }
}

TEST_CASE("special incomplete gamma inverses: extreme tails vs scipy", "[v12-a][special][incomplete]")
{
    // p within 1e-9 of 1 is resolved through the exact 1 - p; q far below 1e-16 used to come back as +inf.
    for (std::size_t i = 0; i < len(ref_gammaincinv_tail_a); ++i)
    {
        chk(sf::gammainc_p_inv(ref_gammaincinv_tail_a[i], ref_gammaincinv_tail_p[i]), ref_gammaincinv_tail_y[i], 1e-9,
            1e-11, "gammaincinv(tail)", i);
    }
    for (std::size_t i = 0; i < len(ref_gammainccinv_tail_a); ++i)
    {
        chk(sf::gammainc_q_inv(ref_gammainccinv_tail_a[i], ref_gammainccinv_tail_q[i]), ref_gammainccinv_tail_y[i],
            1e-9, 1e-11, "gammainccinv(tail)", i);
    }
}

TEST_CASE("special incomplete beta: large shapes vs scipy", "[v12-a][special][incomplete]")
{
    // The prefactor x^a (1-x)^b / B(a,b) cancels ln Gamma terms of order s*ln(s); the complement 1 - X of a fraction
    // near one then carried an error of 1e-13 and more.
    for (std::size_t i = 0; i < len(ref_betainc_big_a); ++i)
    {
        chk(sf::betainc(ref_betainc_big_a[i], ref_betainc_big_b[i], ref_betainc_big_x[i]), ref_betainc_big_y[i], 1e-12,
            1e-14, "betainc(big)", i);
    }
}

TEST_CASE("special incomplete beta inverse: extreme tails vs scipy", "[v12-a][special][incomplete]")
{
    // A steep tail used to stall the unguarded Halley iteration; p within 1e-9 of 1 is resolved through 1 - p.
    for (std::size_t i = 0; i < len(ref_betaincinv_tail_a); ++i)
    {
        chk(sf::betainc_inv(ref_betaincinv_tail_a[i], ref_betaincinv_tail_b[i], ref_betaincinv_tail_p[i]),
            ref_betaincinv_tail_y[i], 1e-9, 1e-11, "betaincinv(tail)", i);
    }
}

TEST_CASE("special incomplete gamma: huge shapes vs exact power sums", "[v12-a][special][incomplete]")
{
    // The series and the fraction need about 8.5*sqrt(a) terms near x = a and their rounding grows with them, so shapes
    // of 1e8 left errors of 1e-9 and shapes of 1e9 and above came out wrong altogether; the uniform expansion (Temme
    // 1979) has no such limit. The references up to 1e8 are exact power sums (scipy's own tails are good to 1e-8
    // there); at x = a scipy's expansion agrees with them to the last digit and serves up to 1e300.
    for (std::size_t i = 0; i < len(ref_gammainc_huge_a); ++i)
    {
        chk(sf::gammainc_p(ref_gammainc_huge_a[i], ref_gammainc_huge_x[i]), ref_gammainc_huge_p[i], 1e-12, 1e-14,
            "gammainc_p(huge)", i);
        chk(sf::gammainc_q(ref_gammainc_huge_a[i], ref_gammainc_huge_x[i]), ref_gammainc_huge_q[i], 1e-12, 1e-14,
            "gammainc_q(huge)", i);
    }
    for (std::size_t i = 0; i < len(ref_gammainc_centre_a); ++i)
    {
        chk(sf::gammainc_p(ref_gammainc_centre_a[i], ref_gammainc_centre_a[i]), ref_gammainc_centre_p[i], 1e-12, 1e-14,
            "gammainc_p(centre)", i);
        chk(sf::gammainc_q(ref_gammainc_centre_a[i], ref_gammainc_centre_a[i]), ref_gammainc_centre_q[i], 1e-12, 1e-14,
            "gammainc_q(centre)", i);
    }
}

TEST_CASE("special incomplete gamma inverses: far lower tail and huge shapes", "[v12-a][special][incomplete]")
{
    // The Wilson-Hilferty start left the Halley iteration many e-folds from a root in the lower tail (p below 1e-14 at
    // a = 7.5, below 1e-300 at a = 1000): wrong values or NaN. Newton's method on ln P reaches them.
    for (std::size_t i = 0; i < len(ref_gammaincinv_low_a); ++i)
    {
        chk(sf::gammainc_p_inv(ref_gammaincinv_low_a[i], ref_gammaincinv_low_p[i]), ref_gammaincinv_low_y[i], 1e-9,
            1e-11, "gammaincinv(low)", i);
    }
    // Huge shapes: scipy's inverse is itself off by 1e-6 there, so the check is the round trip through P, whose
    // residual relative to p is far inside what the 1e-9 gate on x allows.
    for (std::size_t i = 0; i < len(ref_gammaincinv_huge_a); ++i)
    {
        const f64 x = sf::gammainc_p_inv(ref_gammaincinv_huge_a[i], ref_gammaincinv_huge_p[i]);
        const f64 back = sf::gammainc_p(ref_gammaincinv_huge_a[i], x);
        INFO("gammaincinv(huge)[" << i << "] x=" << x << " P(x)=" << back);
        CHECK(std::abs(back / ref_gammaincinv_huge_p[i] - 1.0) < 1e-9);
        if (ref_gammaincinv_huge_p[i] >= 0.01) // 1 - p is exact enough to invert Q only where p is not small
        {
            const f64 xq = sf::gammainc_q_inv(ref_gammaincinv_huge_a[i], 1.0 - ref_gammaincinv_huge_p[i]);
            CHECK(std::abs(xq / x - 1.0) < 1e-9);
        }
    }
}

TEST_CASE("special incomplete beta: far from the mode and huge shapes vs exact sums", "[v12-a][special][incomplete]")
{
    // With both shapes large the prefactor's offset form 1 + u = x*s/a rounded to -1 for a small x, so values of 1e-281
    // came out as 0 or off by factors; the relative check (no absolute floor) holds those values to the module's rtol.
    for (std::size_t i = 0; i < len(ref_betainc_far_a); ++i)
    {
        chk(sf::betainc(ref_betainc_far_a[i], ref_betainc_far_b[i], ref_betainc_far_x[i]), ref_betainc_far_y[i], 1e-12,
            0.0, "betainc(far)", i);
    }
    // Shapes of 1e7 and above: the fraction needs O(sqrt(a+b)) terms whose rounding, and the prefactor's, outgrow the
    // gate (1e-7 at 1e8, wrong at 1e12); the uniform expansion (Temme 1982) does not.
    for (std::size_t i = 0; i < len(ref_betainc_huge_a); ++i)
    {
        chk(sf::betainc(ref_betainc_huge_a[i], ref_betainc_huge_b[i], ref_betainc_huge_x[i]), ref_betainc_huge_y[i],
            1e-12, 1e-14, "betainc(huge)", i);
    }
    // The symmetric case at the mode is exactly one half.
    chk(sf::betainc(1e12, 1e12, 0.5), 0.5, 1e-13, 0.0, "betainc(1e12,1e12,1/2)", 0);
    chk(sf::betainc(1e8, 1e8, 0.5), 0.5, 1e-13, 0.0, "betainc(1e8,1e8,1/2)", 0);
}

TEST_CASE("special incomplete gamma: single precision at a shape of 0.001", "[v12-a][special][incomplete][f32]")
{
    // gammainc_q below a+1 is 1 - P, so the rounding of ln Gamma(a) reaches it with the gain 1/Q: lgamma<float>(0.001)
    // was 6.9071760 for 6.9071789, which left Q 1.3 % off (about 2.8 times the probe limit of 1e-4 relative + 1e-6).
    for (std::size_t i = 0; i < len(ref_gammainc_f32_a); ++i)
    {
        const float got =
            sf::gammainc_q(static_cast<float>(ref_gammainc_f32_a[i]), static_cast<float>(ref_gammainc_f32_x[i]));
        INFO("gammainc_q<float>[" << i << "] got=" << got << " expected=" << ref_gammainc_f32_q[i]);
        CHECK(std::abs(static_cast<f64>(got) - ref_gammainc_f32_q[i]) <= 1e-6 + 1e-4 * std::abs(ref_gammainc_f32_q[i]));
    }
    for (const float x : {0.001F, 0.01F, 0.5F, 1.83268F, 2.5F, 5.0F, 11.9F})
    {
        // Within one unit in the last place of the correctly rounded value (the single-precision evaluation was off by
        // up to 330 of them near the zero of lgamma at 2).
        const float expected = static_cast<float>(std::lgamma(static_cast<f64>(x)));
        const f64 ulp = static_cast<f64>(std::nextafter(std::abs(expected), std::numeric_limits<float>::infinity())) -
                        static_cast<f64>(std::abs(expected));
        INFO("lgamma<float>(" << x << ")");
        CHECK(std::abs(static_cast<f64>(sf::lgamma(x)) - static_cast<f64>(expected)) <= ulp);
    }
}

TEST_CASE("special incomplete beta inverse: far tails of a shape well above 1 vs exact roots",
          "[v12-a][special][incomplete]")
{
    // Far below the root the Halley step is clamped to about 2x/(a-1), x grew by 1 + 2/(a-1) per step inside the
    // bracket and a hundred steps fell short by factors (and NaN); Newton's method on ln I from below, which is
    // concave, does not. The upper tail is the same problem mirrored (x -> 1-x, a <-> b); 1 - p is exact there. The
    // references are the roots in the exact power sums.
    for (std::size_t i = 0; i < len(ref_betaincinv_lowtail_a); ++i)
    {
        chk(sf::betainc_inv(ref_betaincinv_lowtail_a[i], ref_betaincinv_lowtail_b[i], ref_betaincinv_lowtail_p[i]),
            ref_betaincinv_lowtail_y[i], 1e-9, 1e-11, "betaincinv(lower tail)", i);
    }
    for (std::size_t i = 0; i < len(ref_betaincinv_uptail_a); ++i)
    {
        chk(sf::betainc_inv(ref_betaincinv_uptail_a[i], ref_betaincinv_uptail_b[i], ref_betaincinv_uptail_p[i]),
            ref_betaincinv_uptail_y[i], 1e-9, 1e-11, "betaincinv(upper tail)", i);
    }
}

TEST_CASE("special incomplete beta: between the expansions and at shapes of a few hundred vs exact sums",
          "[v12-a][special][incomplete]")
{
    // One shape of a few hundred to a thousand with the other far larger, or both of that size with a sum of 1e4 to
    // 5e5: neither the fraction (whose rounding near the mode grows like sqrt(a+b), R = 1.4 to 6.4 of the gate at the
    // points below) nor the expansions served them.
    for (std::size_t i = 0; i < len(ref_betainc_gap_a); ++i)
    {
        chk(sf::betainc(ref_betainc_gap_a[i], ref_betainc_gap_b[i], ref_betainc_gap_x[i]), ref_betainc_gap_y[i], 1e-12,
            1e-14, "betainc(gap)", i);
        chk(sf::betainc(ref_betainc_gap_a[i], ref_betainc_gap_b[i], ref_betainc_gap_x[i],
                        sf::lbeta(ref_betainc_gap_a[i], ref_betainc_gap_b[i])),
            ref_betainc_gap_y[i], 1e-12, 1e-14, "betainc(gap, lbeta)", i);
    }
    // Shapes of 600 to 1000: the prefactor from three log-gamma terms of size s*ln(s) lost up to 3 times the gate.
    for (std::size_t i = 0; i < len(ref_betainc_mid_a); ++i)
    {
        chk(sf::betainc(ref_betainc_mid_a[i], ref_betainc_mid_b[i], ref_betainc_mid_x[i]), ref_betainc_mid_y[i], 1e-12,
            1e-14, "betainc(mid)", i);
        chk(sf::betainc(ref_betainc_mid_a[i], ref_betainc_mid_b[i], ref_betainc_mid_x[i],
                        sf::lbeta(ref_betainc_mid_a[i], ref_betainc_mid_b[i])),
            ref_betainc_mid_y[i], 1e-12, 1e-14, "betainc(mid, lbeta)", i);
    }
}

TEST_CASE("special incomplete beta: a small shape with x near 1 vs exact sums", "[v12-a][special][incomplete]")
{
    // I below 1e-2 reached as the complement 1 - value of a value near one: the rounding of the prefactor's exponent is
    // scaled by 1/I, and ln B of a shape of 1e-4 (lnGamma = 9.2) came out four ulps off from three lgamma terms of size
    // 600*ln(600) (R = 3 to 22 of the gate at these points) and 4 ulps from the shifted Stirling form of lgamma alone.
    for (std::size_t i = 0; i < len(ref_betainc_edge_a); ++i)
    {
        chk(sf::betainc(ref_betainc_edge_a[i], ref_betainc_edge_b[i], ref_betainc_edge_x[i]), ref_betainc_edge_y[i],
            1e-12, 1e-14, "betainc(edge)", i);
    }
}

TEST_CASE("special lbeta: large shapes vs exact log-gamma", "[v12-a][special][gamma]")
{
    // lnGamma(a) + lnGamma(b) - lnGamma(a+b) cancels terms of size s*ln(s); the Stirling form does not.
    for (std::size_t i = 0; i < len(ref_lbeta_big_a); ++i)
    {
        chk(sf::lbeta(ref_lbeta_big_a[i], ref_lbeta_big_b[i]), ref_lbeta_big_y[i], 2e-15, 1e-15, "lbeta(big)", i);
    }
}

TEST_CASE("special incomplete: single-precision inverses at small shapes vs scipy", "[v12-a][special][incomplete][f32]")
{
    // The single-precision rounding of P(a,x), I_x(a,b) and ln B is divided by the density a*x^(a-1) of a small shape:
    // R reached 1.2 to 3.7 (9.9 with the log-gamma sum in float) of the probe limit of 1e-4 relative + 1e-6.
    for (std::size_t i = 0; i < len(ref_betaincinv_f32_a); ++i)
    {
        const float got =
            sf::betainc_inv(static_cast<float>(ref_betaincinv_f32_a[i]), static_cast<float>(ref_betaincinv_f32_b[i]),
                            static_cast<float>(ref_betaincinv_f32_p[i]));
        INFO("betainc_inv<float>[" << i << "] got=" << got << " expected=" << ref_betaincinv_f32_y[i]);
        CHECK(std::abs(static_cast<f64>(got) - ref_betaincinv_f32_y[i]) <=
              1e-6 + 1e-4 * std::abs(ref_betaincinv_f32_y[i]));
    }
    for (std::size_t i = 0; i < len(ref_gammaincinv_f32_a); ++i)
    {
        const float got = sf::gammainc_p_inv(static_cast<float>(ref_gammaincinv_f32_a[i]),
                                             static_cast<float>(ref_gammaincinv_f32_p[i]));
        INFO("gammainc_p_inv<float>[" << i << "] got=" << got << " expected=" << ref_gammaincinv_f32_y[i]);
        CHECK(std::abs(static_cast<f64>(got) - ref_gammaincinv_f32_y[i]) <=
              1e-6 + 1e-4 * std::abs(ref_gammaincinv_f32_y[i]));
    }
}

TEST_CASE("special incomplete beta: single precision at a small shape vs scipy", "[v12-a][special][incomplete][f32]")
{
    // The prefactor's exponent, ln B = 7 at a shape of 0.001, rounds to 5e-7 in single precision: R = 1.2 at this point
    // (scipy for the float inputs: 0.00042928088).
    const float got = sf::betainc(5.0F, 0.001F, 0.87F);
    INFO("betainc<float>(5, 0.001, 0.87) got=" << got);
    CHECK(std::abs(static_cast<f64>(got) - 0.00042928088) <= 1e-6 + 1e-4 * 0.00042928088);
}

TEST_CASE("special incomplete beta: single precision at sums of 1e10 and more vs exact sums",
          "[v12-a][special][incomplete][f32]")
{
    // A float x cannot resolve the width of the mode against the rounding of the float shapes at these sums: base
    // returned 0, 1 or an infinity. References: the quad-precision continued fraction at the float inputs.
    struct Vector
    {
        float a;
        float b;
        float x;
        f64 expected;
    };
    const Vector vectors[] = {
        {4.31509668e+10F, 947790080.0F, 0.978508949F, 0.97821329205491181},
        {3.14787463e+11F, 7.07548365e+09F, 0.978016913F, 0.24458390912610239},
        {1.9379712e+09F, 5.91161852e+11F, 0.00326746213F, 0.18145755808826958},
        {2.25606492e+10F, 7.46991616e+10F, 0.231962785F, 0.52236020836472652},
        {1.77630085e+11F, 9.66971228e+10F, 0.647512496F, 0.79919839163471529},
        {1.67879868e+11F, 9.96651418e+10F, 0.627482116F, 0.26213083146773086},
        {5.22000486e+09F, 1.19871478e+12F, 0.00433576386F, 0.34926842942725589},
        {6.84199987e+09F, 5.38988347e+11F, 0.012534935F, 0.26375380563206441},
    };
    for (const Vector& v : vectors)
    {
        const float got = sf::betainc(v.a, v.b, v.x);
        INFO("betainc<float>(" << v.a << ", " << v.b << ", " << v.x << ") got=" << got << " expected=" << v.expected);
        CHECK(std::abs(static_cast<f64>(got) - v.expected) <= 1e-6 + 1e-4 * v.expected);
    }
}

TEST_CASE("special incomplete beta: single precision at a smaller shape of 1e4 and more vs exact sums",
          "[v12-a][special][incomplete][f32]")
{
    // The float terms of the expansion in incomplete gamma functions overflow there while the power of 1/b underflows
    // to 0, and inf * 0 gave NaN; the expansion runs in double and narrows. References: the quad-precision continued
    // fraction.
    struct Vector
    {
        float a;
        float b;
        float x;
        f64 expected;
    };
    const Vector vectors[] = {
        {27442.236328125F, 1600622080.0F, 1.6644948118482716e-05F, 5.5238892487704179e-07},
        {9754062848.0F, 64425.33203125F, 0.99999350309371948F, 0.99998495723654812},
        {129985.2890625F, 46778540032.0F, 2.8220256353961304e-06F, 0.99999998861732187},
    };
    for (const Vector& v : vectors)
    {
        const f64 tolerance = 1e-6 + 1e-4 * v.expected;
        const float three = sf::betainc(v.a, v.b, v.x);
        const float four = sf::betainc(v.a, v.b, v.x, sf::lbeta(v.a, v.b));
        INFO("betainc<float>(" << v.a << ", " << v.b << ", " << v.x << ") got=" << three << ", " << four);
        CHECK(std::abs(static_cast<f64>(three) - v.expected) <= tolerance);
        CHECK(std::abs(static_cast<f64>(four) - v.expected) <= tolerance);
    }
    // A shape of the smallest subnormal float: the float expansion's sum is not finite there; I_x is 1 for every x > 0.
    const float tiny = std::numeric_limits<float>::denorm_min();
    for (const float big : {3000.0F, 60000.0F, 1e7F})
    {
        const float got = sf::betainc(tiny, big, 1e-30F);
        INFO("betainc<float>(denorm, " << big << ", 1e-30) got=" << got);
        CHECK(std::abs(got - 1.0F) <= 1e-6F);
    }
}

TEST_CASE("special incomplete beta: a subnormal smaller shape and shapes of one at the largest double",
          "[v12-a][special][incomplete]")
{
    // The expansion in incomplete gamma functions forms x^a/(a·Gamma(a)) through 1/a, which overflows at a subnormal
    // shape (NaN or inf); I_x is 1 there for every x > 0, and its mirror 0.
    const f64 tiny = std::numeric_limits<f64>::denorm_min();
    CHECK(sf::betainc(tiny, 1e4, 1e-6) == 1.0);
    CHECK(sf::betainc(tiny, 1e4, 1e-6, sf::lbeta(tiny, 1e4)) == 1.0);
    CHECK(sf::betainc(1e4, tiny, 1.0 - 1e-6) == 0.0);
    // I_x(1,b) = 1 - (1-x)^b in closed form, and its inverse; the fraction's coefficients overflow at the largest b.
    const f64 big = std::numeric_limits<f64>::max();
    const f64 x = 5.5e-319;
    const f64 forward = sf::betainc(1.0, big, x);
    const f64 forward_expected = -std::expm1(big * std::log1p(-x));
    INFO("betainc(1, DBL_MAX, 5.5e-319) got=" << forward << " expected=" << forward_expected);
    CHECK(std::abs(forward - forward_expected) <= 1e-12 * forward_expected);
    for (const f64 p : {1e-10, 1e-3, 0.25})
    {
        const f64 root = sf::betainc_inv(1.0, big, p);
        const f64 root_expected = -std::expm1(std::log1p(-p) / big);
        INFO("betainc_inv(1, DBL_MAX, " << p << ") got=" << root << " expected=" << root_expected);
        CHECK(std::abs(root - root_expected) <= 1e-9 * root_expected);
    }
    // A subnormal shape: the root lies below every double but the smallest, and the iteration returns a finite one.
    const f64 tiny_root = sf::betainc_inv(tiny, 1e10, 1e-6);
    INFO("betainc_inv(denorm, 1e10, 1e-6) got=" << tiny_root);
    CHECK(tiny_root >= 0.0);
    CHECK(tiny_root <= 1e-11);
}

TEST_CASE("special incomplete: inverses of the second review vs exact roots", "[v12-a][special][incomplete]")
{
    // A lower-tail root at a shape of 324 (exact root 0.77983297590652806 by mpmath at 60 digits).
    const f64 root = sf::betainc_inv(324.17047482198899, 7.3125885387990266, 4.336562208702579e-27);
    CHECK(std::abs(root - 0.77983297590652806) <= 1e-11 + 1e-9 * 0.77983297590652806);
    // Single-precision gamma inverses at a shape of 6e7: float cannot form the density at the mode there (NaN before);
    // references: the double inverse at the float inputs.
    const float gp = sf::gammainc_p_inv(64276868.0F, 4.1486e-15F);
    INFO("gammainc_p_inv<float>(64276868, 4.1486e-15) got=" << gp);
    CHECK(std::abs(static_cast<f64>(gp) - 64214649.9) <= 1e-6 + 1e-4 * 64214649.9);
    const float gq = sf::gammainc_q_inv(64276868.0F, 0.25F);
    const f64 gq_expected = sf::gammainc_q_inv(static_cast<f64>(64276868.0F), 0.25);
    INFO("gammainc_q_inv<float>(64276868, 0.25) got=" << gq << " expected=" << gq_expected);
    CHECK(std::abs(static_cast<f64>(gq) - gq_expected) <= 1e-6 + 1e-4 * gq_expected);
    // Single-precision beta inverses at a smaller shape near and below 1e-6 and through the 4-argument overload;
    // references: the double inverse at the float inputs.
    struct Vector
    {
        float a;
        float b;
        float p;
        f64 expected;
    };
    const Vector vectors[] = {
        {9.9552244137157686e-07F, 288.22216796875F, 0.99999845027923584F, 0.000468636546},
        {1.0341750567022245e-06F, 299.64776611328125F, 0.9999992847442627F, 0.00135791736},
        {0.0033360288944095373F, 0.099862255156040192F, 0.96284472942352295F, 0.163363373},
    };
    for (const Vector& v : vectors)
    {
        const f64 tolerance = 1e-6 + 1e-4 * v.expected;
        const float three = sf::betainc_inv(v.a, v.b, v.p);
        const float four = sf::betainc_inv(v.a, v.b, v.p, sf::lbeta(v.a, v.b));
        INFO("betainc_inv<float>(" << v.a << ", " << v.b << ", " << v.p << ") got=" << three << ", " << four);
        CHECK(std::abs(static_cast<f64>(three) - v.expected) <= tolerance);
        CHECK(std::abs(static_cast<f64>(four) - v.expected) <= tolerance);
    }
}

TEST_CASE("special incomplete: a NaN argument gives NaN", "[v12-a][special][incomplete]")
{
    // The long runs of the series and the fractions are for finite values that converge slowly; a NaN never converges
    // and would run them to their bound (50 times base's cost) before returning the NaN it started as.
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    CHECK(std::isnan(sf::gammainc_p(500.0, nan, sf::lgamma(500.0))));
    CHECK(std::isnan(sf::gammainc_q(500.0, nan, sf::lgamma(500.0))));
    CHECK(std::isnan(sf::gammainc_p(nan, 1.0)));
    CHECK(std::isnan(sf::gammainc_p_inv(nan, 0.5)));
    CHECK(std::isnan(sf::gammainc_q_inv(nan, 1e-20)));
    CHECK(std::isnan(sf::betainc(300.0, 300.0, nan, sf::lbeta(300.0, 300.0))));
    CHECK(std::isnan(sf::betainc(nan, 300.0, 0.5)));
    CHECK(std::isnan(sf::betainc_inv(nan, 300.0, 0.5)));
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

TEST_CASE("special erf: dawson at infinity and past 1e300 (the lattice index overflows from 9e307)",
          "[v12-a][special][erf]")
{
    const f64 inf = std::numeric_limits<f64>::infinity();
    CHECK(sf::dawson(inf) == 0.0);
    CHECK(sf::dawson(-inf) == 0.0);
    CHECK(std::signbit(sf::dawson(-inf)));
    CHECK(std::isnan(sf::dawson(std::numeric_limits<f64>::quiet_NaN())));
    // D(x) = 1/(2x) to every digit there (scipy: 2.941176470588236e-309 at 1.7e308; subnormal results)
    chk(sf::dawson(1.7e308), 2.941176470588236e-309, 1e-12, 0.0, "dawson", 0);
    chk(sf::dawson(-1.0e300), -5.0e-301, 1e-12, 0.0, "dawson", 1);
    chk(sf::dawson(9.5e307), 5.263157894736842e-309, 1e-12, 0.0, "dawson", 2);
    CHECK(sf::dawson(std::numeric_limits<float>::infinity()) == 0.0F);
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
