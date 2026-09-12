// crd-hesap-special v12-c — orthogonal polynomials gated vs scipy.special + analytic identities + f32.

#include <crd/hesap/special/special.hpp>

#include "orthopoly_refs.inc"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

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

TEST_CASE("orthopoly: Legendre/Chebyshev/Gegenbauer/Jacobi vs scipy", "[v12-c][special][orthopoly]")
{
    for (std::size_t i = 0; i < len(ref_leg_y); ++i)
    {
        const int n = static_cast<int>(ref_op_n[i]);
        const f64 x = ref_op_x[i];
        INFO("n=" << n << " x=" << x);
        CHECK(close(sf::legendre(n, x), ref_leg_y[i], 1e-12, 1e-13));
        CHECK(close(sf::chebyshev_t(n, x), ref_cht_y[i], 1e-12, 1e-13));
        CHECK(close(sf::chebyshev_u(n, x), ref_chu_y[i], 1e-12, 1e-13));
        CHECK(close(sf::chebyshev_v(n, x), ref_chv_y[i], 1e-11, 1e-12));
        CHECK(close(sf::chebyshev_w(n, x), ref_chw_y[i], 1e-11, 1e-12));
        CHECK(close(sf::gegenbauer(n, 0.75, x), ref_geg_y[i], 1e-11, 1e-12));
        CHECK(close(sf::jacobi(n, 1.5, 0.5, x), ref_jac_y[i], 1e-11, 1e-12));
    }
}

TEST_CASE("orthopoly: Hermite (physicist + probabilist) vs scipy", "[v12-c][special][orthopoly]")
{
    for (std::size_t i = 0; i < len(ref_her_y); ++i)
    {
        const int n = static_cast<int>(ref_her_n[i]);
        const f64 x = ref_her_x[i];
        INFO("n=" << n << " x=" << x);
        CHECK(close(sf::hermite(n, x), ref_her_y[i], 1e-11, 1e-12));
        CHECK(close(sf::hermite_e(n, x), ref_hee_y[i], 1e-11, 1e-12));
    }
}

TEST_CASE("orthopoly: Laguerre + generalized vs scipy", "[v12-c][special][orthopoly]")
{
    for (std::size_t i = 0; i < len(ref_lag_y); ++i)
    {
        const int n = static_cast<int>(ref_lag_n[i]);
        const f64 x = ref_lag_x[i];
        INFO("n=" << n << " x=" << x);
        CHECK(close(sf::laguerre(n, x), ref_lag_y[i], 1e-11, 1e-12));
        CHECK(close(sf::laguerre_assoc(n, 1.5, x), ref_glag_y[i], 1e-11, 1e-12));
    }
}

TEST_CASE("orthopoly: associated Legendre vs scipy lpmv", "[v12-c][special][orthopoly]")
{
    for (std::size_t i = 0; i < len(ref_alp_y); ++i)
    {
        const int m = static_cast<int>(ref_alp_m[i]);
        const int n = static_cast<int>(ref_alp_n[i]);
        const f64 x = ref_alp_x[i];
        INFO("m=" << m << " n=" << n << " x=" << x);
        CHECK(close(sf::legendre_assoc(n, m, x), ref_alp_y[i], 1e-11, 1e-12));
    }
}

TEST_CASE("orthopoly: analytic identities + f32", "[v12-c][special][orthopoly]")
{
    // T_n(cos θ) = cos(n θ); U_n(cos θ) = sin((n+1)θ)/sin θ
    for (int n = 0; n <= 7; ++n)
    {
        for (const f64 th : {0.3, 1.0, 2.0, 2.8})
        {
            const f64 x = std::cos(th);
            CHECK(close(sf::chebyshev_t(n, x), std::cos(n * th), 1e-12, 1e-13));
            CHECK(close(sf::chebyshev_u(n, x), std::sin((n + 1) * th) / std::sin(th), 1e-11, 1e-12));
        }
    }
    CHECK(close(sf::legendre(2, 0.5), -0.125, 1e-13, 1e-14));      // P₂(½) = (3·¼−1)/2 = −1/8
    CHECK(close(static_cast<f64>(sf::legendre(3, 0.4F)), -0.44, 1e-6, 1e-7)); // P₃(0.4) = (5·0.064−3·0.4)/2
    CHECK(close(static_cast<f64>(sf::hermite(4, 1.0F)), -20.0, 1e-5, 1e-6));  // H₄(1) = 16−48+12 = −20
}
