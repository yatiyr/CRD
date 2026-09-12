// test_hyperdual.cpp — Phase 3.1.6 v15-c: exact second-order forward AD. Gates: Hessians match the analytic
// reference (polynomial + transcendental); symmetry; curvature vᵀHv (one pass) == vᵀ·H·v; the flat HyperDual second
// derivative == nested Dual<Dual> == analytic; and the no-cancellation property (hyper-dual f'' is exact where
// FD-of-FD loses ~half the digits).

#include <crd/hesap/autodiff/forward.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath> // std:: reference oracle

using Catch::Matchers::WithinRel;

namespace ad = crd::hesap::autodiff::forward;
using HD     = ad::HyperDual<double>;

namespace
{
struct Poly2 // f = x0²·x1 + 3·x0·x1²
{
    template <class T>
    T operator()(const T* x, int /*n*/) const
    {
        return x[0] * x[0] * x[1] + 3.0 * x[0] * x[1] * x[1];
    }
};
struct Trans2 // f = exp(x0)·sin(x1) + x0·x1
{
    template <class T>
    T operator()(const T* x, int /*n*/) const
    {
        using std::exp;
        using std::sin;
        return exp(x[0]) * sin(x[1]) + x[0] * x[1];
    }
};
struct Quad // f = 3x0² + 2x0·x1 + 4x1² − 5x0 − 6x1 ; g = [6x0+2x1−5, 2x0+8x1−6] ; H = [[6,2],[2,8]]
{
    template <class T>
    T operator()(const T* x, int /*n*/) const
    {
        return 3.0 * x[0] * x[0] + 2.0 * x[0] * x[1] + 4.0 * x[1] * x[1] - 5.0 * x[0] - 6.0 * x[1];
    }
};
} // namespace

TEST_CASE("HyperDual Hessian matches analytic (polynomial)", "[autodiff][hyperdual]")
{
    const double x[2] = {1.3, 0.7};
    double       h[4];
    ad::hessian<2>(Poly2{}, x, h);
    // H = [[2·x1, 2·x0+6·x1], [2·x0+6·x1, 6·x0]]
    CHECK_THAT(h[0], WithinRel(2.0 * x[1], 1e-12));
    CHECK_THAT(h[1], WithinRel(2.0 * x[0] + 6.0 * x[1], 1e-12));
    CHECK_THAT(h[2], WithinRel(2.0 * x[0] + 6.0 * x[1], 1e-12));
    CHECK_THAT(h[3], WithinRel(6.0 * x[0], 1e-12));
    CHECK(h[1] == h[2]); // symmetry, exactly
}

TEST_CASE("HyperDual Hessian matches analytic (transcendental)", "[autodiff][hyperdual]")
{
    const double x[2] = {0.5, 0.9};
    double       h[4];
    ad::hessian<2>(Trans2{}, x, h);
    const double e = std::exp(x[0]);
    // H00 = e·sin(x1); H01 = e·cos(x1)+1; H11 = -e·sin(x1)
    CHECK_THAT(h[0], WithinRel(e * std::sin(x[1]), 1e-12));
    CHECK_THAT(h[1], WithinRel(e * std::cos(x[1]) + 1.0, 1e-12));
    CHECK_THAT(h[3], WithinRel(-e * std::sin(x[1]), 1e-12));
    CHECK(h[1] == h[2]);
}

TEST_CASE("HyperDual curvature vT-H-v equals vT * H * v in ONE pass", "[autodiff][hyperdual]")
{
    const double x[2] = {0.5, 0.9};
    const double v[2] = {0.6, -0.4};
    double       h[4];
    ad::hessian<2>(Trans2{}, x, h);
    const double vhv = v[0] * v[0] * h[0] + 2.0 * v[0] * v[1] * h[1] + v[1] * v[1] * h[3];
    CHECK_THAT(ad::curvature<2>(Trans2{}, x, v), WithinRel(vhv, 1e-12));
}

TEST_CASE("Flat HyperDual f'' == nested Dual<Dual> == analytic (arithmetic)", "[autodiff][hyperdual][nested]")
{
    // g(t) = t³ − 2t² + t ;  g''(t) = 6t − 4.  Written without raw-scalar multiplies so the SAME lambda deduces for
    // both HyperDual<double> and the nested Dual<Dual<double>> (a `double · Dual<Dual>` can't deduce the mixed op).
    auto g = [](auto t) { return t * t * t - t * t - t * t + t; };
    const double t0 = 1.7;

    // flat HyperDual: seed both ε on the single variable → f12 = g''
    const HD hy      = g(HD{t0, 1.0, 1.0, 0.0});
    const double g2_flat = hy.f12;

    // nested Dual<Dual>: seed value.d and deriv.v → result.d.d = g''
    using DD             = ad::Dual<ad::Dual<double>>;
    const DD td          = DD{ad::Dual<double>{t0, 1.0}, ad::Dual<double>{1.0, 0.0}};
    const DD yd          = g(td);
    const double g2_nested = yd.d.d;

    CHECK_THAT(g2_flat, WithinRel(6.0 * t0 - 4.0, 1e-12));
    CHECK_THAT(g2_nested, WithinRel(6.0 * t0 - 4.0, 1e-12));
    CHECK_THAT(g2_flat, WithinRel(g2_nested, 1e-13));
    // and the value + first derivative come out too
    CHECK_THAT(hy.f0, WithinRel(t0 * t0 * t0 - 2.0 * t0 * t0 + t0, 1e-13));
    CHECK_THAT(hy.f1, WithinRel(3.0 * t0 * t0 - 4.0 * t0 + 1.0, 1e-12)); // g'
}

TEST_CASE("HyperDual second derivative is EXACT where FD-of-FD is not", "[autodiff][hyperdual]")
{
    // f(t) = exp(sin(t)); f''(t) = exp(sin(t))·(cos²(t) − sin(t))
    auto f = [](auto t) {
        using std::exp;
        using std::sin;
        return exp(sin(t));
    };
    const double t0    = 1.1;
    const double exact = std::exp(std::sin(t0)) * (std::cos(t0) * std::cos(t0) - std::sin(t0));

    const double hd = f(HD{t0, 1.0, 1.0, 0.0}).f12; // exact 2nd derivative

    // FD-of-FD (central of central), the naive peer — loses ~half the digits
    const double h    = 1e-4;
    const double fdfd = (f(t0 + h) - 2.0 * f(t0) + f(t0 - h)) / (h * h);

    CHECK_THAT(hd, WithinRel(exact, 1e-12));  // hyper-dual: full precision
    CHECK_THAT(fdfd, WithinRel(exact, 1e-3)); // FD-of-FD: only ~3 digits (documents the cancellation it avoids)
    CHECK(std::abs(hd - exact) < std::abs(fdfd - exact));
}

TEST_CASE("Exact-Hessian Newton step reaches a quadratic minimum in ONE step (opt gate)", "[autodiff][hyperdual][opt]")
{
    const double x[2] = {0.0, 0.0};

    // gradient via Jet (one pass); Hessian via the hyper-dual driver (exact).
    ad::Jet<double, 2> jx[2] = {ad::Jet<double, 2>(x[0], 0), ad::Jet<double, 2>(x[1], 1)};
    const auto         y     = Quad{}(jx, 2);
    const double       g[2]  = {y.v[0], y.v[1]};
    double             h[4];
    ad::hessian<2>(Quad{}, x, h);

    // Newton step  dx = −H⁻¹ g  (2×2 solve); x + dx must be the exact minimizer.
    const double det = h[0] * h[3] - h[1] * h[2];
    const double dx0 = -(h[3] * g[0] - h[1] * g[1]) / det;
    const double dx1 = -(-h[2] * g[0] + h[0] * g[1]) / det;
    // analytic minimizer x* = H⁻¹ b, b = [5,6] → [28/44, 26/44]
    CHECK_THAT(x[0] + dx0, WithinRel(28.0 / 44.0, 1e-12));
    CHECK_THAT(x[1] + dx1, WithinRel(26.0 / 44.0, 1e-12));
}

TEST_CASE("HyperDual transcendental slopes (g'' surface)", "[autodiff][hyperdual]")
{
    const double t = 0.4;
    auto         second = [](const HD& r) { return r.f12; }; // with ε1=ε2=1 seed, f12 = g''(t)
    auto         hd     = [t]() { return HD{t, 1.0, 1.0, 0.0}; };
    CHECK_THAT(second(ad::sin(hd())), WithinRel(-std::sin(t), 1e-12));
    CHECK_THAT(second(ad::cos(hd())), WithinRel(-std::cos(t), 1e-12));
    CHECK_THAT(second(ad::exp(hd())), WithinRel(std::exp(t), 1e-12));
    CHECK_THAT(second(ad::log(hd())), WithinRel(-1.0 / (t * t), 1e-12));
    CHECK_THAT(second(ad::sqrt(hd())), WithinRel(-0.25 * std::pow(t, -1.5), 1e-12));
    CHECK_THAT(second(ad::tanh(hd())), WithinRel(-2.0 * std::tanh(t) * (1.0 - std::tanh(t) * std::tanh(t)), 1e-12));
    CHECK_THAT(second(ad::atan(hd())), WithinRel(-2.0 * t / ((1.0 + t * t) * (1.0 + t * t)), 1e-12));
    CHECK_THAT(second(ad::sinh(hd())), WithinRel(std::sinh(t), 1e-12));
}
