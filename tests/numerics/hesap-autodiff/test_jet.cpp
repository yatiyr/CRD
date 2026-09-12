// test_jet.cpp — Phase 3.1.6 v15-a: the Jet<T,N> forward carrier gate. All N directional derivatives in ONE pass;
// the ★Jet<T,1> == Dual<T> BIT-EXACT identity (the two carriers share the same closed forms — a correctness AND
// determinism cross-check); a full Jacobian recovered in a single pass; mixed ops; select/min/max/abs; constexpr.
// Reference values from std math (the v15-a test oracle; the 3-oracle complex-step/FD gate is v15-b). This target
// links ONLY crd-hesap-autodiff (with test_dual_forward.cpp) = the link-isolation smoke.

#include <crd/hesap/autodiff/forward.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace ad = crd::hesap::autodiff::forward;

using D  = ad::Dual<double>;
using J1 = ad::Jet<double, 1>;
using J3 = ad::Jet<double, 3>;

TEST_CASE("Jet seed constructor sets exactly one tangent", "[autodiff][jet]")
{
    const J3 x{2.0, 0};
    CHECK(x.a == 2.0);
    CHECK(x.v[0] == 1.0);
    CHECK(x.v[1] == 0.0);
    CHECK(x.v[2] == 0.0);
    CHECK(J3::DIMENSION == 3);
}

TEST_CASE("Jet arithmetic propagates every partial", "[autodiff][jet]")
{
    const J3 x{3.0, 0};
    const J3 y{5.0, 1};

    const J3 s = x + y;
    CHECK(s.a == 8.0);
    CHECK(s.v[0] == 1.0);
    CHECK(s.v[1] == 1.0);
    CHECK(s.v[2] == 0.0);

    const J3 p = x * y; // d/dx = y = 5, d/dy = x = 3
    CHECK(p.a == 15.0);
    CHECK(p.v[0] == 5.0);
    CHECK(p.v[1] == 3.0);

    const J3 q = x / y; // d/dx = 1/y = 0.2, d/dy = -x/y^2 = -0.12
    CHECK_THAT(q.a, WithinRel(0.6, 1e-15));
    CHECK_THAT(q.v[0], WithinRel(0.2, 1e-15));
    CHECK_THAT(q.v[1], WithinRel(-3.0 / 25.0, 1e-15));
}

TEST_CASE("Jet transcendentals match the analytic derivative", "[autodiff][jet]")
{
    const J1 x{0.6, 0};
    CHECK_THAT(ad::sin(x).v[0], WithinRel(std::cos(0.6), 1e-13));
    CHECK_THAT(ad::cos(x).v[0], WithinRel(-std::sin(0.6), 1e-13));
    CHECK_THAT(ad::exp(x).v[0], WithinRel(std::exp(0.6), 1e-13));
    CHECK_THAT(ad::log(J1{2.0, 0}).v[0], WithinRel(0.5, 1e-13));
    CHECK_THAT(ad::sqrt(J1{4.0, 0}).v[0], WithinRel(0.25, 1e-13));
    CHECK_THAT(ad::tanh(x).v[0], WithinRel(1.0 - std::tanh(0.6) * std::tanh(0.6), 1e-12));
    CHECK_THAT(ad::pow(J1{3.0, 0}, 2.0).v[0], WithinRel(6.0, 1e-13));
    CHECK_THAT(ad::abs(J1{-2.0, 0}).v[0], WithinRel(-1.0, 1e-14));
}

TEST_CASE("Jet of width 1 equals Dual bit-for-bit", "[autodiff][jet][dual][determinism]")
{
    // The two carriers use the identical closed forms; every shared op must agree to the last bit.
    // Reproducing this exactly is both a correctness statement AND the scalar-level determinism guarantee.
    const double x0 = 0.7;
    const D       xd{x0, 1.0};
    const J1      xj{x0, 0};

    auto same = [](const D& d, const J1& j) {
        CHECK_THAT(j.a, WithinAbs(d.v, 0.0));
        CHECK_THAT(j.v[0], WithinAbs(d.d, 0.0));
    };

    same(ad::sin(xd), ad::sin(xj));
    same(ad::cos(xd), ad::cos(xj));
    same(ad::tan(xd), ad::tan(xj));
    same(ad::exp(xd), ad::exp(xj));
    same(ad::log(xd), ad::log(xj));
    same(ad::sqrt(xd), ad::sqrt(xj));
    same(ad::tanh(xd), ad::tanh(xj));
    same(ad::pow(xd, 2.5), ad::pow(xj, 2.5));
    // A composed expression: exp(sin(x)) / (1 + x*x).
    same(ad::exp(ad::sin(xd)) / (1.0 + xd * xd), ad::exp(ad::sin(xj)) / (1.0 + xj * xj));
    // Mixed scalar operators on both sides.
    same(2.0 - xd, 2.0 - xj);
    same(2.0 / xd, 2.0 / xj);
    same(xd / 4.0, xj / 4.0);
}

TEST_CASE("Jet computes a full gradient in a single pass", "[autodiff][jet]")
{
    // f(x,y,z) = x*y + sin(z); grad = (y, x, cos(z)).
    const J3 x{1.5, 0};
    const J3 y{2.0, 1};
    const J3 z{0.3, 2};
    const J3 f = x * y + ad::sin(z);
    CHECK_THAT(f.a, WithinRel(1.5 * 2.0 + std::sin(0.3), 1e-14));
    CHECK_THAT(f.v[0], WithinRel(2.0, 1e-14));            // df/dx = y
    CHECK_THAT(f.v[1], WithinRel(1.5, 1e-14));            // df/dy = x
    CHECK_THAT(f.v[2], WithinRel(std::cos(0.3), 1e-13));  // df/dz = cos(z)
}

TEST_CASE("Jet recovers a dense Jacobian in one pass", "[autodiff][jet]")
{
    // F: R^2 -> R^2, F0 = x0^2 + x1, F1 = x0*x1. J = [[2 x0, 1],[x1, x0]].
    using J2 = ad::Jet<double, 2>;
    const double x0 = 3.0;
    const double x1 = 4.0;
    const J2     a{x0, 0};
    const J2     b{x1, 1};

    const J2 f0 = a * a + b;
    const J2 f1 = a * b;

    // Row 0 = grad F0, row 1 = grad F1.
    CHECK_THAT(f0.v[0], WithinRel(2.0 * x0, 1e-14)); // dF0/dx0
    CHECK_THAT(f0.v[1], WithinAbs(1.0, 1e-14));      // dF0/dx1
    CHECK_THAT(f1.v[0], WithinRel(x1, 1e-14));       // dF1/dx0
    CHECK_THAT(f1.v[1], WithinRel(x0, 1e-14));       // dF1/dx1
}

TEST_CASE("Jet mixed scalar operators", "[autodiff][jet]")
{
    const J1 x{2.0, 0};
    CHECK_THAT((3.0 * x).v[0], WithinRel(3.0, 1e-15));
    CHECK_THAT((x * 3.0).v[0], WithinRel(3.0, 1e-15));
    CHECK_THAT((5.0 - x).v[0], WithinRel(-1.0, 1e-15)); // d/dx (5 - x) = -1
    CHECK_THAT((x - 5.0).v[0], WithinRel(1.0, 1e-15));
    CHECK_THAT((10.0 / x).v[0], WithinRel(-10.0 / 4.0, 1e-15)); // -s/x^2
    CHECK_THAT((x / 4.0).v[0], WithinRel(0.25, 1e-15));
}

TEST_CASE("Jet select min max carry the active branch", "[autodiff][jet][rules]")
{
    const J1 a{2.0, 0};
    const J1 b{5.0, 0}; // seed both to dir 0 so branch selection is what we observe
    const J1 bd{5.0, 0};
    CHECK(ad::select(true, a, bd).a == 2.0);
    CHECK(ad::select(false, a, bd).a == 5.0);
    CHECK(ad::min(a, b).a == 2.0);
    CHECK(ad::max(a, b).a == 5.0);
    // tie -> first argument
    const J1 c{4.0, 0};
    const J1 e{4.0, 0};
    CHECK(ad::min(c, e).a == 4.0);
    CHECK(ad::max(c, e).a == 4.0);
}

TEST_CASE("Jet arithmetic is constexpr and matches runtime bit-for-bit", "[autodiff][jet][determinism]")
{
    // Compile-time evaluation must equal the runtime path exactly (a determinism guarantee for the pure-arith core).
    constexpr J1 cx{3.0, 0};
    constexpr J1 cf = cx * cx + 2.0 * cx; // value 15, derivative 2x+2 = 8
    STATIC_REQUIRE(cf.a == 15.0);
    STATIC_REQUIRE(cf.v[0] == 8.0);

    const J1 rx{3.0, 0};
    const J1 rf = rx * rx + 2.0 * rx;
    CHECK_THAT(rf.a, WithinAbs(cf.a, 0.0));
    CHECK_THAT(rf.v[0], WithinAbs(cf.v[0], 0.0));
}
