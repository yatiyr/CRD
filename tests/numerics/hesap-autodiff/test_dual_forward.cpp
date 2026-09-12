// test_dual_forward.cpp — Phase 3.1.6 v15-a: the forward substrate gate. Dual<T> exact first derivatives (the
// migrated v7-b surface), Jet<T,N> computing all partials in ONE pass, Jet==Dual equivalence per direction, the
// select/min/max active-branch rules, and the DiffFunctor concept. Reference values from std math (the test-side
// oracle; the full 3-oracle analytic/complex-step/FD gate lands with the rule library in v15-b).

#include <crd/hesap/autodiff/forward.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace ad = crd::hesap::autodiff::forward;

using D = ad::Dual<double>;

TEST_CASE("Dual carries exact first derivatives", "[autodiff][dual]")
{
    // Polynomial: d/dx (x*x) = 2x at x=3 -> 6; value 9.
    const D x{3.0, 1.0};
    const D sq = x * x;
    CHECK_THAT(sq.v, WithinRel(9.0, 1e-15));
    CHECK_THAT(sq.d, WithinRel(6.0, 1e-15));

    // Transcendentals (chain rule, tangent seeded to 1).
    CHECK_THAT(ad::sin(D{0.5, 1.0}).d, WithinRel(std::cos(0.5), 1e-13));
    CHECK_THAT(ad::cos(D{0.5, 1.0}).d, WithinRel(-std::sin(0.5), 1e-13));
    CHECK_THAT(ad::exp(D{1.0, 1.0}).d, WithinRel(std::exp(1.0), 1e-13));
    CHECK_THAT(ad::log(D{2.0, 1.0}).d, WithinRel(0.5, 1e-13));  // 1/x
    CHECK_THAT(ad::sqrt(D{4.0, 1.0}).d, WithinRel(0.25, 1e-13)); // 1/(2*sqrt(x))
    CHECK_THAT(ad::tan(D{0.3, 1.0}).d, WithinRel(1.0 / (std::cos(0.3) * std::cos(0.3)), 1e-12));
    CHECK_THAT(ad::tanh(D{0.4, 1.0}).d, WithinRel(1.0 - std::tanh(0.4) * std::tanh(0.4), 1e-12));
    CHECK_THAT(ad::pow(D{3.0, 1.0}, 2.0).d, WithinRel(6.0, 1e-13)); // p*x^(p-1) = 2*3
    CHECK_THAT(ad::abs(D{-2.0, 1.0}).d, WithinRel(-1.0, 1e-14));    // sign(x)*x'
}

TEST_CASE("Jet computes all partials in one pass", "[autodiff][jet]")
{
    using J = ad::Jet<double, 2>;
    // f(x,y) = x*x + x*y at (x=10, y=20): f=300, df/dx=2x+y=40, df/dy=x=10.
    const J x{10.0, 0};
    const J y{20.0, 1};
    const J f = x * x + x * y;
    CHECK_THAT(f.a, WithinRel(300.0, 1e-15));
    CHECK_THAT(f.v[0], WithinRel(40.0, 1e-15));
    CHECK_THAT(f.v[1], WithinRel(10.0, 1e-15));
}

TEST_CASE("Jet single direction equals Dual", "[autodiff][jet][dual]")
{
    // f(x) = exp(sin(x)) at x=0.7; the Jet<double,1> and Dual<double> paths share the exact same closed forms.
    const double x0 = 0.7;
    const D      xd{x0, 1.0};
    const D      fd = ad::exp(ad::sin(xd));

    using J1 = ad::Jet<double, 1>;
    const J1 xj{x0, 0};
    const J1 fj = ad::exp(ad::sin(xj));

    CHECK_THAT(fj.a, WithinRel(fd.v, 1e-15));
    CHECK_THAT(fj.v[0], WithinRel(fd.d, 1e-15));
    // and both match the analytic derivative cos(x)*exp(sin(x)).
    CHECK_THAT(fd.d, WithinRel(std::cos(x0) * std::exp(std::sin(x0)), 1e-13));
}

TEST_CASE("select min max carry the active branch derivative", "[autodiff][rules]")
{
    const D a{2.0, 3.0};
    const D b{5.0, 7.0};

    CHECK(ad::select(true, a, b).d == 3.0);
    CHECK(ad::select(false, a, b).d == 7.0);

    // min picks the smaller value's branch (2 < 5 -> a); max the larger (b).
    CHECK_THAT(ad::min(a, b).v, WithinAbs(2.0, 0.0));
    CHECK_THAT(ad::min(a, b).d, WithinAbs(3.0, 0.0));
    CHECK_THAT(ad::max(a, b).v, WithinAbs(5.0, 0.0));
    CHECK_THAT(ad::max(a, b).d, WithinAbs(7.0, 0.0));

    // Documented tie convention: min/max return the FIRST argument on an exact value tie.
    const D c{4.0, 1.0};
    const D e{4.0, 9.0};
    CHECK_THAT(ad::min(c, e).d, WithinAbs(1.0, 0.0));
    CHECK_THAT(ad::max(c, e).d, WithinAbs(1.0, 0.0));
}

TEST_CASE("Dual chain rule through composed expressions", "[autodiff][dual]")
{
    // g(x) = log(1 + x*x); g'(x) = 2x/(1+x*x).
    {
        const double x0 = 1.3;
        const D       g = ad::log(1.0 + D{x0, 1.0} * D{x0, 1.0});
        CHECK_THAT(g.v, WithinRel(std::log(1.0 + x0 * x0), 1e-13));
        CHECK_THAT(g.d, WithinRel(2.0 * x0 / (1.0 + x0 * x0), 1e-13));
    }
    // h(x) = sqrt(x*x + 1); h'(x) = x/sqrt(x*x+1).
    {
        const double x0 = 2.0;
        const D       x{x0, 1.0};
        const D       h = ad::sqrt(x * x + 1.0);
        CHECK_THAT(h.d, WithinRel(x0 / std::sqrt(x0 * x0 + 1.0), 1e-13));
    }
    // k(x) = tanh(exp(x)); k'(x) = (1 - tanh(e^x)^2) * e^x.
    {
        const double x0 = 0.2;
        const D       k = ad::tanh(ad::exp(D{x0, 1.0}));
        const double  e = std::exp(x0);
        CHECK_THAT(k.d, WithinRel((1.0 - std::tanh(e) * std::tanh(e)) * e, 1e-12));
    }
    // product rule via sin*cos; (sin cos)' = cos^2 - sin^2.
    {
        const double x0 = 0.9;
        const D       x{x0, 1.0};
        const D       r = ad::sin(x) * ad::cos(x);
        CHECK_THAT(r.d, WithinRel(std::cos(x0) * std::cos(x0) - std::sin(x0) * std::sin(x0), 1e-13));
    }
}

TEST_CASE("Dual pow with a dual exponent", "[autodiff][dual]")
{
    // f(t) = x(t)^y(t) with x=t, y=t at t=2: value 4; d/dt = x^y (y' ln x + y x'/x) = 4 (ln 2 + 1).
    const D x{2.0, 1.0};
    const D y{2.0, 1.0};
    const D f = ad::pow(x, y);
    CHECK_THAT(f.v, WithinRel(4.0, 1e-14));
    CHECK_THAT(f.d, WithinRel(4.0 * (std::log(2.0) + 1.0), 1e-13));
}

TEST_CASE("Dual mixed scalar operators", "[autodiff][dual]")
{
    const D x{3.0, 1.0};
    CHECK_THAT((2.0 + x).d, WithinAbs(1.0, 0.0));
    CHECK_THAT((x + 2.0).d, WithinAbs(1.0, 0.0));
    CHECK_THAT((2.0 - x).d, WithinAbs(-1.0, 0.0));
    CHECK_THAT((x - 2.0).d, WithinAbs(1.0, 0.0));
    CHECK_THAT((4.0 * x).d, WithinAbs(4.0, 0.0));
    CHECK_THAT((x * 4.0).d, WithinAbs(4.0, 0.0));
    CHECK_THAT((x / 2.0).d, WithinRel(0.5, 1e-15));
    CHECK_THAT((6.0 / x).d, WithinRel(-6.0 / 9.0, 1e-15)); // -s/x^2
}

TEST_CASE("Dual comparisons act on the value", "[autodiff][dual]")
{
    const D a{1.0, 9.0}; // huge tangent must not influence ordering
    const D b{2.0, -9.0};
    CHECK(a < b);
    CHECK(b > a);
    CHECK(a <= a);
    CHECK(a == D{1.0, -3.0}); // equal value, different tangent -> equal
    CHECK(a != b);
}

TEST_CASE("Dual arithmetic is constexpr", "[autodiff][dual]")
{
    constexpr D x{5.0, 1.0};
    constexpr D f = x * x - 3.0 * x; // value 10, derivative 2x-3 = 7
    STATIC_REQUIRE(f.v == 10.0);
    STATIC_REQUIRE(f.d == 7.0);
}

namespace
{
// A scalar-generic functor (the DiffFunctor contract): one templated operator() runs on T and on Dual<T>.
struct Quadratic
{
    template <class S>
    S operator()(crd::containers::ConstSpan<S> x) const
    {
        return x[0] * x[0] + x[1];
    }
};
} // namespace

TEST_CASE("DiffFunctor concept accepts a scalar-generic functor", "[autodiff][concept]")
{
    STATIC_REQUIRE(ad::DiffFunctor<Quadratic, double>);
    STATIC_REQUIRE(ad::DiffFunctor<Quadratic, float>);
}
