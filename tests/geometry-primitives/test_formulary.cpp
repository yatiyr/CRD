// crd-geometry-primitives v0e -- the iq formulary (smooth-min/max + domain ops).
// Verifies the canonical properties: the poly smins are exact min outside the
// blend band and collapse to min as k->0; all smins dip below min at the
// crossover; smax = -smin(-*,-*); domain_repeat/mirror are periodic and stay in
// the centered cell; domain_elongate zeroes inside +-h; domain_twist/bend are
// rigid (preserve the perpendicular radius and the untouched axis).

#include <crd/geometry/primitives/formulary.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace crd;
using namespace crd::math;
using namespace crd::geometry::primitives;

namespace
{
template <typename T> constexpr T tol() noexcept
{
    return std::is_same_v<T, float> ? static_cast<T>(1e-4) : static_cast<T>(1e-9);
}
} // namespace

TEMPLATE_TEST_CASE("v0e -- smin / smax (polynomial & cubic)", "[geometry][formulary]", float, double)
{
    using T = TestType;
    const T k = static_cast<T>(0.5);

    SECTION("exact min/max outside the blend band (|a-b| >= 4k for poly, >= 6k for cubic)")
    {
        REQUIRE(smin_poly<T>(1, 5, k) == Catch::Approx(static_cast<T>(1)).margin(tol<T>()));
        REQUIRE(smax_poly<T>(1, 5, k) == Catch::Approx(static_cast<T>(5)).margin(tol<T>()));
        REQUIRE(smin_cubic<T>(1, 5, k) == Catch::Approx(static_cast<T>(1)).margin(tol<T>()));
        REQUIRE(smax_cubic<T>(1, 5, k) == Catch::Approx(static_cast<T>(5)).margin(tol<T>()));
        REQUIRE(smin_poly<T>(-3, 7, k) == Catch::Approx(static_cast<T>(-3)).margin(tol<T>()));
    }
    SECTION("dips below min at the crossover; smax rises above max")
    {
        REQUIRE(smin_poly<T>(2, 2, k) < static_cast<T>(2));
        REQUIRE(smin_cubic<T>(2, 2, k) < static_cast<T>(2));
        REQUIRE(smax_poly<T>(2, 2, k) > static_cast<T>(2));
        // Max dip of the quadratic poly smin at the crossover (a==b, h==1) is exactly k.
        REQUIRE(smin_poly<T>(2, 2, k) == Catch::Approx(static_cast<T>(2) - k).margin(tol<T>()));
        REQUIRE(smax_poly<T>(2, 2, k) == Catch::Approx(static_cast<T>(2) + k).margin(tol<T>()));
    }
    SECTION("k -> 0 collapses to plain min / max")
    {
        const T tiny = static_cast<T>(1e-6);
        REQUIRE(smin_poly<T>(2, static_cast<T>(2.0001), tiny) ==
                Catch::Approx(static_cast<T>(2)).margin(static_cast<T>(1e-3)));
        REQUIRE(smin_cubic<T>(2, static_cast<T>(2.0001), tiny) ==
                Catch::Approx(static_cast<T>(2)).margin(static_cast<T>(1e-3)));
        REQUIRE(smax_poly<T>(2, static_cast<T>(2.0001), tiny) ==
                Catch::Approx(static_cast<T>(2.0001)).margin(static_cast<T>(1e-3)));
    }
    SECTION("symmetry and the smax/smin duality")
    {
        REQUIRE(smin_poly<T>(static_cast<T>(1.3), static_cast<T>(1.1), k) ==
                Catch::Approx(smin_poly<T>(static_cast<T>(1.1), static_cast<T>(1.3), k)).margin(tol<T>()));
        REQUIRE(smax_poly<T>(static_cast<T>(0.7), static_cast<T>(1.0), k) ==
                Catch::Approx(-smin_poly<T>(static_cast<T>(-0.7), static_cast<T>(-1.0), k)).margin(tol<T>()));
        REQUIRE(smax_cubic<T>(static_cast<T>(0.7), static_cast<T>(1.0), k) ==
                Catch::Approx(-smin_cubic<T>(static_cast<T>(-0.7), static_cast<T>(-1.0), k)).margin(tol<T>()));
    }
    SECTION("smin always <= min(a, b)")
    {
        for (int i = -20; i <= 20; ++i)
        {
            const T a = static_cast<T>(i) * static_cast<T>(0.1);
            for (int j = -20; j <= 20; ++j)
            {
                const T b = static_cast<T>(j) * static_cast<T>(0.1);
                const T m = a < b ? a : b;
                REQUIRE(smin_poly<T>(a, b, k) <= m + tol<T>());
                REQUIRE(smin_cubic<T>(a, b, k) <= m + tol<T>());
            }
        }
    }
}

TEMPLATE_TEST_CASE("v0e -- smin_exp / smax_exp", "[geometry][formulary]", float, double)
{
    using T = TestType;
    SECTION("dips below min; <= min(a,b); symmetry; the smax duality")
    {
        const T k = static_cast<T>(0.05);
        REQUIRE(smin_exp<T>(static_cast<T>(0.02), static_cast<T>(0.02), k) < static_cast<T>(0.02));
        REQUIRE(smin_exp<T>(static_cast<T>(0.01), static_cast<T>(0.03), k) <= static_cast<T>(0.01) + tol<T>());
        REQUIRE(smin_exp<T>(static_cast<T>(0.01), static_cast<T>(0.03), k) ==
                Catch::Approx(smin_exp<T>(static_cast<T>(0.03), static_cast<T>(0.01), k)).margin(tol<T>()));
        REQUIRE(smax_exp<T>(static_cast<T>(0.01), static_cast<T>(0.03), k) ==
                Catch::Approx(-smin_exp<T>(static_cast<T>(-0.01), static_cast<T>(-0.03), k)).margin(tol<T>()));
    }
    SECTION("converges toward min as k shrinks (within the numerically-safe range)")
    {
        const T a = static_cast<T>(0.01);
        const T b = static_cast<T>(0.03);
        const T big = smin_exp<T>(a, b, static_cast<T>(0.05));
        const T small = smin_exp<T>(a, b, static_cast<T>(0.005));
        REQUIRE(small <= a + tol<T>());
        REQUIRE((a - small) < (a - big) + tol<T>()); // closer to min with the smaller k
        REQUIRE(small == Catch::Approx(a).margin(static_cast<T>(1e-3)));
    }
}

TEMPLATE_TEST_CASE("v0e -- value-domain ops: op_round / op_onion / extrude_2d", "[geometry][formulary]", float, double)
{
    using T = TestType;
    REQUIRE(op_round<T>(static_cast<T>(2.0), static_cast<T>(0.3)) ==
            Catch::Approx(static_cast<T>(1.7)).margin(tol<T>()));
    REQUIRE(op_onion<T>(static_cast<T>(2.0), static_cast<T>(0.5)) ==
            Catch::Approx(static_cast<T>(1.5)).margin(tol<T>()));
    REQUIRE(op_onion<T>(static_cast<T>(-2.0), static_cast<T>(0.5)) ==
            Catch::Approx(static_cast<T>(1.5)).margin(tol<T>()));
    REQUIRE(op_onion<T>(static_cast<T>(0.2), static_cast<T>(0.5)) ==
            Catch::Approx(static_cast<T>(-0.3)).margin(tol<T>()));

    SECTION("extrude_2d turns a 2D circle SDF into a cylinder")
    {
        // sdf2d of a unit circle at the origin = length(p.xy) - 1; extrude to half-height h = 2.
        const T h = static_cast<T>(2);
        const auto cyl = [&](T px, T py, T pz)
        {
            return extrude_2d<T>(static_cast<T>(std::sqrt(px * px + py * py)) - static_cast<T>(1), pz, h);
        };
        REQUIRE(cyl(0, 0, 0) == Catch::Approx(static_cast<T>(-1)).margin(tol<T>())); // deep inside
        REQUIRE(cyl(3, 0, 0) == Catch::Approx(static_cast<T>(2)).margin(tol<T>()));  // 2 units past the side wall (r=1)
        REQUIRE(cyl(0, 0, 4) == Catch::Approx(static_cast<T>(2)).margin(tol<T>()));  // 2 units past the top cap (h=2)
        REQUIRE(cyl(0, 0, static_cast<T>(1)) ==
                Catch::Approx(static_cast<T>(-1)).margin(tol<T>())); // inside, 1 below the cap
        // diagonal: at radial distance 1 past the wall AND 1 past the cap -> length((1,1)) = sqrt2
        REQUIRE(cyl(2, 0, 3) == Catch::Approx(static_cast<T>(std::sqrt(static_cast<T>(2)))).margin(tol<T>()));
    }
}

TEMPLATE_TEST_CASE("v0e -- domain_repeat / domain_mirror", "[geometry][formulary]", float, double)
{
    using T = TestType;
    const T period = static_cast<T>(3);
    SECTION("repeat is periodic and lands in the centered cell")
    {
        for (int i = -7; i <= 7; ++i)
        {
            const T p = static_cast<T>(i) * static_cast<T>(0.37);
            const T q = domain_repeat<T>(p, period);
            REQUIRE(q >= -period * static_cast<T>(0.5) - tol<T>());
            REQUIRE(q < period * static_cast<T>(0.5) + tol<T>());
            REQUIRE(domain_repeat<T>(p + period, period) == Catch::Approx(q).margin(tol<T>()));
            REQUIRE(domain_repeat<T>(p - static_cast<T>(2) * period, period) == Catch::Approx(q).margin(tol<T>()));
        }
        // Vec2/Vec3 componentwise
        const Vec2<T> q2 = domain_repeat<T>(Vec2<T>(static_cast<T>(4), static_cast<T>(-5)), Vec2<T>(period, period));
        REQUIRE(q2.x == Catch::Approx(domain_repeat<T>(static_cast<T>(4), period)).margin(tol<T>()));
        REQUIRE(q2.y == Catch::Approx(domain_repeat<T>(static_cast<T>(-5), period)).margin(tol<T>()));
        const Vec3<T> q3 = domain_repeat<T>(Vec3<T>(static_cast<T>(7), static_cast<T>(0.5), static_cast<T>(-9)),
                                            Vec3<T>(period, period, period));
        REQUIRE(q3.z == Catch::Approx(domain_repeat<T>(static_cast<T>(-9), period)).margin(tol<T>()));
    }
    SECTION("mirror reflects at the cell boundary and stays in the centered cell")
    {
        const T eps = static_cast<T>(0.05);
        for (int i = -7; i <= 7; ++i)
        {
            const T p = static_cast<T>(i) * static_cast<T>(0.41);
            const T q = domain_mirror<T>(p, period);
            REQUIRE(q >= -period * static_cast<T>(0.5) - tol<T>());
            REQUIRE(q <= period * static_cast<T>(0.5) + tol<T>());
            // 2*period is the mirror super-period
            REQUIRE(domain_mirror<T>(p + static_cast<T>(2) * period, period) == Catch::Approx(q).margin(tol<T>()));
        }
        // straddling x = period: domain_mirror(period - eps) and domain_mirror(period + eps) reflect to the same value
        REQUIRE(domain_mirror<T>(period - eps, period) ==
                Catch::Approx(domain_mirror<T>(period + eps, period)).margin(tol<T>()));
        // straddling x = 0 similarly (mirror about a cell wall)
        REQUIRE(domain_mirror<T>(-eps, period) == Catch::Approx(domain_mirror<T>(eps, period)).margin(tol<T>()));
    }
}

TEMPLATE_TEST_CASE("v0e -- domain_elongate / domain_twist / domain_bend", "[geometry][formulary]", float, double)
{
    using T = TestType;
    SECTION("elongate: zero inside +-h, p - clamp outside")
    {
        const Vec3<T> h(static_cast<T>(1), static_cast<T>(2), static_cast<T>(0.5));
        const Vec3<T> in = domain_elongate<T>(Vec3<T>(static_cast<T>(0.5), static_cast<T>(-1), static_cast<T>(0.2)), h);
        REQUIRE(in.x == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));
        REQUIRE(in.y == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));
        REQUIRE(in.z == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));
        const Vec3<T> out = domain_elongate<T>(Vec3<T>(static_cast<T>(3), static_cast<T>(-5), static_cast<T>(0.4)), h);
        REQUIRE(out.x == Catch::Approx(static_cast<T>(2)).margin(tol<T>()));  // 3 - clamp(3, -1, 1) = 3 - 1
        REQUIRE(out.y == Catch::Approx(static_cast<T>(-3)).margin(tol<T>())); // -5 - clamp(-5, -2, 2) = -5 + 2
        REQUIRE(out.z == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));  // 0.4 within +-0.5 -> 0
        const Vec2<T> out2 = domain_elongate<T>(Vec2<T>(static_cast<T>(4), static_cast<T>(0.1)),
                                                Vec2<T>(static_cast<T>(1.5), static_cast<T>(1)));
        REQUIRE(out2.x == Catch::Approx(static_cast<T>(2.5)).margin(tol<T>()));
        REQUIRE(out2.y == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));
    }
    SECTION("twist: identity at k=0; rigid (preserves p.y and the xz-radius)")
    {
        const Vec3<T> p(static_cast<T>(2), static_cast<T>(3), static_cast<T>(-1));
        const Vec3<T> q0 = domain_twist<T>(p, static_cast<T>(0));
        REQUIRE(q0.x == Catch::Approx(p.x).margin(tol<T>()));
        REQUIRE(q0.y == Catch::Approx(p.y).margin(tol<T>()));
        REQUIRE(q0.z == Catch::Approx(p.z).margin(tol<T>()));
        const Vec3<T> q = domain_twist<T>(p, static_cast<T>(0.7));
        REQUIRE(q.y == Catch::Approx(p.y).margin(tol<T>()));
        REQUIRE((q.x * q.x + q.z * q.z) == Catch::Approx(p.x * p.x + p.z * p.z).margin(static_cast<T>(2e-3)));
    }
    SECTION("bend: identity at k=0; rigid (preserves p.z and the xy-radius)")
    {
        const Vec3<T> p(static_cast<T>(1.5), static_cast<T>(-2), static_cast<T>(4));
        const Vec3<T> q0 = domain_bend<T>(p, static_cast<T>(0));
        REQUIRE(q0.x == Catch::Approx(p.x).margin(tol<T>()));
        REQUIRE(q0.z == Catch::Approx(p.z).margin(tol<T>()));
        const Vec3<T> q = domain_bend<T>(p, static_cast<T>(0.4));
        REQUIRE(q.z == Catch::Approx(p.z).margin(tol<T>()));
        REQUIRE((q.x * q.x + q.y * q.y) == Catch::Approx(p.x * p.x + p.y * p.y).margin(static_cast<T>(2e-3)));
    }
}
