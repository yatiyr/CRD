// test_jvp_rules.cpp — Phase 3.1.6 v15-b: the full crd::math JVP rule surface + the 3-oracle gate + NaN/inf
// hardening. Each new rule's slope is checked against an independent std:: reference; whole functors are checked
// analytic ≡ complex-step ≡ FD; pow/sqrt/abs edge conventions are pinned (Ceres-faithful); Jet ≡ Dual.

#include <crd/hesap/autodiff/forward.hpp>
#include <crd/hesap/autodiff/gradient_check.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath> // std:: reference oracle (tests may use std; the math-mandate guard covers engine code only)

using Catch::Matchers::WithinRel;

namespace ad  = crd::hesap::autodiff::forward;
namespace adt = crd::hesap::autodiff::testing;
using D       = ad::Dual<double>;

// Runtime value (defeats constant folding). Real AD inputs are always runtime variables; only the degenerate
// ALL-CONSTANT pow(0,0) triggers an MSVC /O2 constant-fold bug on the 0·∞ edge (debug/asan/gcc + all runtime paths
// are correct). Feeding edge inputs through rt() tests the real runtime code path.
[[nodiscard]] static double rt(double v) noexcept
{
    volatile double vv = v;
    return vv;
}

// ------------------------------------------------------------------ per-rule slopes (vs std:: reference)
TEST_CASE("Dual unary JVP slopes match the analytic reference", "[autodiff][jvp]")
{
    const double x = 0.37; // inside every domain (|x|<1 for asin/atanh, x>1 handled separately)
    auto         d = [](const D& r) { return r.d; };

    CHECK_THAT(d(ad::asin(D{x, 1.0})), WithinRel(1.0 / std::sqrt(1.0 - x * x), 1e-12));
    CHECK_THAT(d(ad::acos(D{x, 1.0})), WithinRel(-1.0 / std::sqrt(1.0 - x * x), 1e-12));
    CHECK_THAT(d(ad::atan(D{x, 1.0})), WithinRel(1.0 / (1.0 + x * x), 1e-12));
    CHECK_THAT(d(ad::sinh(D{x, 1.0})), WithinRel(std::cosh(x), 1e-12));
    CHECK_THAT(d(ad::cosh(D{x, 1.0})), WithinRel(std::sinh(x), 1e-12));
    CHECK_THAT(d(ad::asinh(D{x, 1.0})), WithinRel(1.0 / std::sqrt(x * x + 1.0), 1e-12));
    CHECK_THAT(d(ad::atanh(D{x, 1.0})), WithinRel(1.0 / (1.0 - x * x), 1e-12));
    CHECK_THAT(d(ad::exp2(D{x, 1.0})), WithinRel(std::exp2(x) * std::log(2.0), 1e-12));
    CHECK_THAT(d(ad::exp10(D{x, 1.0})), WithinRel(std::pow(10.0, x) * std::log(10.0), 1e-12));
    CHECK_THAT(d(ad::expm1(D{x, 1.0})), WithinRel(std::exp(x), 1e-12));
    CHECK_THAT(d(ad::log2(D{x, 1.0})), WithinRel(1.0 / (x * std::log(2.0)), 1e-12));
    CHECK_THAT(d(ad::log10(D{x, 1.0})), WithinRel(1.0 / (x * std::log(10.0)), 1e-12));
    CHECK_THAT(d(ad::log1p(D{x, 1.0})), WithinRel(1.0 / (1.0 + x), 1e-12));
    CHECK_THAT(d(ad::cbrt(D{x, 1.0})), WithinRel(1.0 / (3.0 * std::cbrt(x) * std::cbrt(x)), 1e-12));
    CHECK_THAT(d(ad::rsqrt(D{x, 1.0})), WithinRel(-0.5 * std::pow(x, -1.5), 1e-12));

    const double xg = 1.7; // domain x>1 for acosh
    CHECK_THAT(d(ad::acosh(D{xg, 1.0})), WithinRel(1.0 / std::sqrt(xg * xg - 1.0), 1e-12));
}

TEST_CASE("Dual binary JVP rules (atan2, hypot)", "[autodiff][jvp]")
{
    const double yv = 0.6;
    const double xv = 0.8;
    // atan2(y,x): ∂/∂y = x/(x²+y²), ∂/∂x = -y/(x²+y²)
    CHECK_THAT(ad::atan2(D{yv, 1.0}, D{xv, 0.0}).d, WithinRel(xv / (xv * xv + yv * yv), 1e-12));
    CHECK_THAT(ad::atan2(D{yv, 0.0}, D{xv, 1.0}).d, WithinRel(-yv / (xv * xv + yv * yv), 1e-12));
    // hypot(x,y): ∂/∂x = x/h
    const double h = std::hypot(xv, yv);
    CHECK_THAT(ad::hypot(D{xv, 1.0}, D{yv, 0.0}).d, WithinRel(xv / h, 1e-12));
    CHECK_THAT(ad::hypot(D{xv, 1.0}, D{yv, 1.0}).d, WithinRel((xv + yv) / h, 1e-12));
}

// ------------------------------------------------------------------ hardened pow / sqrt / abs edges
TEST_CASE("Hardened pow edge conventions (Ceres-faithful)", "[autodiff][jvp][hardening]")
{
    // (0,0) is a genuine 0·∞ singularity: value x^0 ≡ 1; slope NaN (Ceres-faithful — Ceres returns NaN here too)
    CHECK(ad::pow(D{rt(0.0), 1.0}, rt(0.0)).v == 1.0);
    CHECK(std::isnan(ad::pow(D{rt(0.0), 1.0}, rt(0.0)).d));
    // 0^p, p>1 → value 0, slope 0
    CHECK(ad::pow(D{rt(0.0), 1.0}, rt(2.0)).v == 0.0);
    CHECK(ad::pow(D{rt(0.0), 1.0}, rt(2.0)).d == 0.0);
    // 0^1 → value 0, slope 1
    CHECK(ad::pow(D{rt(0.0), 1.0}, rt(1.0)).d == 1.0);
    // 0^p, 0<p<1 → slope +∞
    CHECK(std::isinf(ad::pow(D{rt(0.0), 1.0}, rt(0.5)).d));
    // negative base, integer exponent: value (-2)^3 = -8, base-slope 3·(-2)² = 12
    CHECK_THAT(ad::pow(D{rt(-2.0), 1.0}, rt(3.0)).v, WithinRel(-8.0, 1e-12));
    CHECK_THAT(ad::pow(D{rt(-2.0), 1.0}, rt(3.0)).d, WithinRel(12.0, 1e-12));

    // dual exponent, x>0: d/dt x^y matches f·(y'·ln x + y·x'/x)
    const double xv = 1.5;
    const double yv = 2.3;
    const double f  = std::pow(xv, yv);
    CHECK_THAT(ad::pow(D{xv, 1.0}, D{yv, 0.0}).d, WithinRel(f * yv / xv, 1e-11));      // vary base
    CHECK_THAT(ad::pow(D{xv, 0.0}, D{yv, 1.0}).d, WithinRel(f * std::log(xv), 1e-11)); // vary exp
    // x<0 integer exp: base-slope defined, exponent-slope NaN
    CHECK(std::isnan(ad::pow(D{rt(-2.0), 0.0}, D{rt(3.0), 1.0}).d));
}

TEST_CASE("sqrt/abs boundary conventions", "[autodiff][jvp][hardening]")
{
    CHECK(std::isinf(ad::sqrt(D{rt(0.0), 1.0}).d)); // sqrt'(0) = +∞ (Ceres convention)
    CHECK(ad::abs(D{rt(0.0), 1.0}).d == 1.0);       // subgradient at 0 → +1 branch
    CHECK(ad::abs(D{rt(-3.0), 1.0}).d == -1.0);
}

// ------------------------------------------------------------------ the 3-oracle gate
namespace
{
// Holomorphic (complex-step valid): exp/sin/cos/log/sqrt/tanh + arithmetic.
struct Holo
{
    template <class T>
    T operator()(const T* x, int n) const
    {
        // `using std::` (NOT crd::math): ADL still routes Jet -> forward:: (the rules under test), while
        // std::complex / double use std:: — avoiding the crd::math-vs-std ambiguity on std::complex.
        using std::cos;
        using std::exp;
        using std::log;
        using std::sin;
        using std::sqrt;
        using std::tanh;
        T acc = x[0] * x[0];
        for (int i = 0; i < n; ++i)
        {
            const T& xn = x[(i + 1) % n];
            acc         = acc + exp(sin(x[i])) * sqrt(2.0 + x[i] * xn) + log(3.0 + tanh(x[i])) + cos(x[i] * xn);
        }
        return acc;
    }
};
// Inverse-trig/hyperbolic (no crd::math complex versions → FD-only oracle).
struct Inv
{
    template <class T>
    T operator()(const T* x, int n) const
    {
        using std::asinh;
        using std::atan;
        T acc = T(0.0);
        for (int i = 0; i < n; ++i)
        {
            acc = acc + atan(x[i]) + asinh(x[i] * x[(i + 1) % n]);
        }
        return acc;
    }
};
} // namespace

TEST_CASE("3-oracle gate: analytic == complex-step == FD (holomorphic)", "[autodiff][jvp][oracle]")
{
    constexpr int n     = 5;
    const double  x[n]  = {0.31, 0.52, 0.17, 0.44, 0.28};
    double        ga[n] = {};
    double        gc[n] = {};
    double        gf[n] = {};
    adt::grad_analytic<n>(Holo{}, x, ga);
    adt::grad_cstep<n>(Holo{}, x, gc);
    adt::grad_fd<n>(Holo{}, x, gf);
    CHECK(adt::max_abs_diff<n>(ga, gc) < 1e-10); // analytic vs complex-step (exact oracle)
    CHECK(adt::max_abs_diff<n>(ga, gf) < 1e-5);  // analytic vs FD (independent impl)
}

TEST_CASE("2-oracle gate: analytic == FD (inverse funcs)", "[autodiff][jvp][oracle]")
{
    constexpr int n     = 4;
    const double  x[n]  = {0.3, -0.4, 0.55, -0.2};
    double        ga[n] = {};
    double        gf[n] = {};
    adt::grad_analytic<n>(Inv{}, x, ga);
    adt::grad_fd<n>(Inv{}, x, gf);
    CHECK(adt::max_abs_diff<n>(ga, gf) < 1e-5);
}

// ------------------------------------------------------------------ Jet ≡ Dual on the new surface
TEST_CASE("Jet<T,N> new-surface slopes equal Dual", "[autodiff][jvp]")
{
    const double x = 0.42;
    // seed Jet direction 0; its slot-0 partial must equal the Dual derivative.
    auto j0 = [](const ad::Jet<double, 2>& r) { return r.v[0]; };
    CHECK_THAT(j0(ad::asin(ad::Jet<double, 2>(x, 0))), WithinRel(ad::asin(D{x, 1.0}).d, 1e-13));
    CHECK_THAT(j0(ad::atan(ad::Jet<double, 2>(x, 0))), WithinRel(ad::atan(D{x, 1.0}).d, 1e-13));
    CHECK_THAT(j0(ad::sinh(ad::Jet<double, 2>(x, 0))), WithinRel(ad::sinh(D{x, 1.0}).d, 1e-13));
    CHECK_THAT(j0(ad::log1p(ad::Jet<double, 2>(x, 0))), WithinRel(ad::log1p(D{x, 1.0}).d, 1e-13));
    CHECK_THAT(j0(ad::cbrt(ad::Jet<double, 2>(x, 0))), WithinRel(ad::cbrt(D{x, 1.0}).d, 1e-13));
    // Jet dual-exponent pow matches Dual
    CHECK_THAT(ad::pow(ad::Jet<double, 2>(1.5, 0), ad::Jet<double, 2>(2.3, 1)).a, WithinRel(std::pow(1.5, 2.3), 1e-12));
}
