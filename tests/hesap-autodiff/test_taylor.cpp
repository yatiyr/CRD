// test_taylor.cpp — Phase 3.1.6 v15-g: Taylor-mode jets + the Taylor ODE integrator. Gates: the normalized
// coefficients match the analytic Taylor series (exp / geometric / sin / sqrt-binomial / tanh); the ODE integrator
// reproduces known closed-form solutions to high accuracy in FEW steps (the high-order win).

#include <crd/hesap/autodiff/forward.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace ad = crd::hesap::autodiff::forward;

namespace
{
struct ExpRhs // y' = y        → y = y0·eᵗ
{
    template <class TJ>
    TJ operator()(const TJ& y, const TJ& /*t*/) const { return y; }
};
struct LogisticRhs // y' = y(1−y)   → y = 1/(1 + e^{−t}) for y0=1/2
{
    template <class TJ>
    TJ operator()(const TJ& y, const TJ& /*t*/) const { return y * (1.0 - y); }
};
struct DecayCosRhs // y' = −y + cos(t)  (smooth, forced)
{
    template <class TJ>
    TJ operator()(const TJ& y, const TJ& t) const { return -y + ad::cos(t); }
};
} // namespace

TEST_CASE("TaylorJet coefficients == analytic series", "[autodiff][taylor]")
{
    constexpr int order =8;
    using TJ        = ad::TaylorJet<double, order>;

    // exp(t) at t0=0.5: a[k] = e^{0.5}/k!
    const auto e    = ad::exp(TJ::var(0.5));
    double     fact = 1.0;
    for (int k = 0; k <= order; ++k)
    {
        CHECK_THAT(e.a[k], WithinRel(std::exp(0.5) / fact, 1e-12));
        fact *= static_cast<double>(k + 1);
    }
    // 1/(1−t) at 0: a[k] = 1 (geometric)
    const auto g = TJ(1.0) / (TJ(1.0) - TJ::var(0.0));
    for (int k = 0; k <= order; ++k) { CHECK_THAT(g.a[k], WithinRel(1.0, 1e-12)); }
    // sin(t) at 0: a = 0, 1, 0, −1/6, 0, 1/120, …
    const auto s = ad::sin(TJ::var(0.0));
    CHECK_THAT(s.a[0], WithinAbs(0.0, 1e-14));
    CHECK_THAT(s.a[1], WithinRel(1.0, 1e-12));
    CHECK_THAT(s.a[3], WithinRel(-1.0 / 6.0, 1e-12));
    CHECK_THAT(s.a[5], WithinRel(1.0 / 120.0, 1e-12));
    // √(1+t) at 0: binomial C(1/2,k) → 1, 1/2, −1/8, 1/16, …
    const auto q = ad::sqrt(TJ(1.0) + TJ::var(0.0));
    CHECK_THAT(q.a[0], WithinRel(1.0, 1e-12));
    CHECK_THAT(q.a[1], WithinRel(0.5, 1e-12));
    CHECK_THAT(q.a[2], WithinRel(-0.125, 1e-12));
    CHECK_THAT(q.a[3], WithinRel(0.0625, 1e-12));
    // tanh(t) at 0: a = 0, 1, 0, −1/3, 0, 2/15, …
    const auto th = ad::tanh(TJ::var(0.0));
    CHECK_THAT(th.a[1], WithinRel(1.0, 1e-12));
    CHECK_THAT(th.a[3], WithinRel(-1.0 / 3.0, 1e-12));
    CHECK_THAT(th.a[5], WithinRel(2.0 / 15.0, 1e-12));
    // log(1+t) at 0: a[k] = (−1)^{k+1}/k
    const auto lg = ad::log(TJ(1.0) + TJ::var(0.0));
    CHECK_THAT(lg.a[1], WithinRel(1.0, 1e-12));
    CHECK_THAT(lg.a[2], WithinRel(-0.5, 1e-12));
    CHECK_THAT(lg.a[3], WithinRel(1.0 / 3.0, 1e-12));
    // raw derivative recovery: d³/dt³ sin(t)|₀ = −1
    CHECK_THAT(s.derivative(3), WithinRel(-1.0, 1e-12));
}

TEST_CASE("Taylor ODE integrator: y'=y -> e in FEW steps (high order)", "[autodiff][taylor][ode]")
{
    constexpr int order  = 16;
    int           nsteps = 0;
    const double  y      = ad::taylor_solve<order>(ExpRhs{}, 0.0, 1.0, 1.0, 1e-12, &nsteps);
    CHECK_THAT(y, WithinRel(std::exp(1.0), 1e-10));
    CHECK(nsteps <= 3); // order-16 covers [0,1] in a couple steps
}

TEST_CASE("Taylor ODE integrator: logistic + forced-decay match closed form", "[autodiff][taylor][ode]")
{
    // logistic y' = y(1−y), y0=1/2 → y(t) = 1/(1+e^{−t}); at t=2
    const double yl = ad::taylor_solve<14>(LogisticRhs{}, 0.0, 0.5, 2.0, 1e-12);
    CHECK_THAT(yl, WithinRel(1.0 / (1.0 + std::exp(-2.0)), 1e-9));

    // y' = −y + cos(t), y0=0 → y(t) = ½(sin t + cos t − e^{−t}); at t=3
    const double yd  = ad::taylor_solve<14>(DecayCosRhs{}, 0.0, 0.0, 3.0, 1e-12);
    const double ref = 0.5 * (std::sin(3.0) + std::cos(3.0) - std::exp(-3.0));
    CHECK_THAT(yd, WithinRel(ref, 1e-8));
}

TEST_CASE("Taylor TAPE integrator (O(K^2)/step) == closed form == generic", "[autodiff][taylor][ode]")
{
    // forced decay: taped result matches the closed form AND the generic O(K^3) integrator (same recurrences).
    const double ref     = 0.5 * (std::sin(3.0) + std::cos(3.0) - std::exp(-3.0));
    const double taped   = ad::taylor_solve_taped<14>(DecayCosRhs{}, 0.0, 0.0, 3.0, 1e-12);
    const double generic = ad::taylor_solve<14>(DecayCosRhs{}, 0.0, 0.0, 3.0, 1e-12);
    CHECK_THAT(taped, WithinRel(ref, 1e-9));
    CHECK_THAT(taped, WithinRel(generic, 1e-11));
    // logistic + exponential through the tape
    CHECK_THAT(ad::taylor_solve_taped<14>(LogisticRhs{}, 0.0, 0.5, 2.0, 1e-12),
               WithinRel(1.0 / (1.0 + std::exp(-2.0)), 1e-9));
    CHECK_THAT(ad::taylor_solve_taped<16>(ExpRhs{}, 0.0, 1.0, 1.0, 1e-12), WithinRel(std::exp(1.0), 1e-10));
    // adaptive-order dispatch
    CHECK_THAT(ad::taylor_solve_taped_auto(DecayCosRhs{}, 0.0, 0.0, 3.0, 1e-10), WithinRel(ref, 1e-8));
}
