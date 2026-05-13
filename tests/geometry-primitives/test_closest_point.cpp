// crd-geometry-primitives v0b -- closest-point catalogue (2D + 3D) + segment<->
// segment. Per shape: point already on/inside -> identity; point outside -> on the
// boundary at the analytic distance; degenerates. Triangle: one explicit case
// per Voronoi region + a "minimises over a barycentric sample" property test.
// Segment<->segment: parallel / skew / touching / one-inside-the-other's-span.

#include <crd/geometry/primitives/closest_point.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/math.hpp>

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

template <typename T> void close3(const Vec3<T>& a, const Vec3<T>& b, T eps = tol<T>())
{
    REQUIRE(approx_equal_abs(a.x, b.x, eps));
    REQUIRE(approx_equal_abs(a.y, b.y, eps));
    REQUIRE(approx_equal_abs(a.z, b.z, eps));
}
template <typename T> void close2(const Vec2<T>& a, const Vec2<T>& b, T eps = tol<T>())
{
    REQUIRE(approx_equal_abs(a.x, b.x, eps));
    REQUIRE(approx_equal_abs(a.y, b.y, eps));
}

// splitmix64 -- deterministic per-test sampling.
struct Rng
{
    u64 s;
    explicit Rng(u64 seed) noexcept : s(seed) {}
    u64 next() noexcept
    {
        u64 z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    template <typename T> T uni(T lo, T hi) noexcept
    {
        const T u = static_cast<T>(next() >> 11) * (static_cast<T>(1) / static_cast<T>(1ULL << 53));
        return lo + u * (hi - lo);
    }
};

template <typename T> Vec3<T> rnd3(Rng& r, T lo, T hi)
{
    return Vec3<T>(r.uni(lo, hi), r.uni(lo, hi), r.uni(lo, hi));
}
template <typename T> Vec2<T> rnd2(Rng& r, T lo, T hi)
{
    return Vec2<T>(r.uni(lo, hi), r.uni(lo, hi));
}
} // namespace

// ===========================================================================
// 3D
// ===========================================================================

TEMPLATE_TEST_CASE("closest_point 3D -- linear primitives", "[geometry][closest_point]", float, double)
{
    using T = TestType;

    SECTION("Line3 -- projection, no clamp")
    {
        const Line3<T> line(Vec3<T>(1, 2, 3), Vec3<T>(0, 0, 2)); // along +z
        close3(closest_point(line, Vec3<T>(5, 2, 100)), Vec3<T>(1, 2, 100));
        close3(closest_point(line, Vec3<T>(1, 2, -7)), Vec3<T>(1, 2, -7)); // behind the anchor -- still on the line
        REQUIRE(distance(line, Vec3<T>(1, 6, 50)) == Catch::Approx(static_cast<T>(4)).margin(tol<T>()));
    }
    SECTION("Segment3 -- clamps to the endpoints")
    {
        const Segment3<T> seg(Vec3<T>(0, 0, 0), Vec3<T>(10, 0, 0));
        close3(closest_point(seg, Vec3<T>(3, 4, 0)), Vec3<T>(3, 0, 0));   // interior
        close3(closest_point(seg, Vec3<T>(-5, 1, 0)), Vec3<T>(0, 0, 0));  // before a
        close3(closest_point(seg, Vec3<T>(99, 1, 0)), Vec3<T>(10, 0, 0)); // past b
        REQUIRE(closest_param(seg, Vec3<T>(3, 4, 0)) == Catch::Approx(static_cast<T>(0.3)).margin(tol<T>()));
        // zero-length segment collapses to its anchor
        const Segment3<T> deg(Vec3<T>(7, 7, 7), Vec3<T>(7, 7, 7));
        close3(closest_point(deg, Vec3<T>(0, 0, 0)), Vec3<T>(7, 7, 7));
    }
    SECTION("Ray3 -- clamps at the origin only")
    {
        const Ray3<T> ray(Vec3<T>(0, 0, 0), Vec3<T>(1, 0, 0));
        close3(closest_point(ray, Vec3<T>(5, 3, 0)), Vec3<T>(5, 0, 0));
        close3(closest_point(ray, Vec3<T>(-9, 3, 0)), Vec3<T>(0, 0, 0)); // behind the origin -> origin
    }
}

TEMPLATE_TEST_CASE("closest_point 3D -- Triangle3 (Ericson Voronoi regions)", "[geometry][closest_point]", float, double)
{
    using T = TestType;
    // unit right triangle in the z=0 plane: a=(0,0,0) b=(1,0,0) c=(0,1,0)
    const Triangle3<T> tri(Vec3<T>(0, 0, 0), Vec3<T>(1, 0, 0), Vec3<T>(0, 1, 0));

    SECTION("vertex regions")
    {
        close3(closest_point(tri, Vec3<T>(-2, -3, 5)), Vec3<T>(0, 0, 0)); // A
        close3(closest_point(tri, Vec3<T>(4, -2, -1)), Vec3<T>(1, 0, 0)); // B
        close3(closest_point(tri, Vec3<T>(-3, 5, 2)), Vec3<T>(0, 1, 0));  // C
    }
    SECTION("edge regions")
    {
        close3(closest_point(tri, Vec3<T>(static_cast<T>(0.5), -4, 0)), Vec3<T>(static_cast<T>(0.5), 0, 0)); // AB
        close3(closest_point(tri, Vec3<T>(-4, static_cast<T>(0.5), 0)), Vec3<T>(0, static_cast<T>(0.5), 0)); // AC
        // BC region: point off the hypotenuse, outside the triangle
        const Vec3<T> q = closest_point(tri, Vec3<T>(2, 2, 0));
        close3(q, Vec3<T>(static_cast<T>(0.5), static_cast<T>(0.5), 0));
    }
    SECTION("face region -- projects straight down")
    {
        close3(closest_point(tri, Vec3<T>(static_cast<T>(0.25), static_cast<T>(0.25), 9)),
               Vec3<T>(static_cast<T>(0.25), static_cast<T>(0.25), 0));
        REQUIRE(distance(tri, Vec3<T>(static_cast<T>(0.25), static_cast<T>(0.25), 9)) ==
                Catch::Approx(static_cast<T>(9)).margin(tol<T>()));
    }
    SECTION("degenerate (collinear) triangle falls back to the edge")
    {
        const Triangle3<T> flat(Vec3<T>(0, 0, 0), Vec3<T>(2, 0, 0), Vec3<T>(5, 0, 0)); // all on the x-axis
        close3(closest_point(flat, Vec3<T>(static_cast<T>(1), 3, 0)), Vec3<T>(static_cast<T>(1), 0, 0));
        close3(closest_point(flat, Vec3<T>(-2, 1, 0)), Vec3<T>(0, 0, 0));
        close3(closest_point(flat, Vec3<T>(99, 1, 0)), Vec3<T>(5, 0, 0));
    }
    SECTION("property: minimises distance over a dense barycentric sample")
    {
        const Triangle3<T> t2(Vec3<T>(-1, 0, 1), Vec3<T>(2, -1, 0), Vec3<T>(0, 3, -2));
        Rng rng(0xC10527U);
        for (int it = 0; it < 64; ++it)
        {
            const Vec3<T> p = rnd3<T>(rng, static_cast<T>(-4), static_cast<T>(4));
            const Vec3<T> q = closest_point(t2, p);
            const T best = distance_squared(t2, p);
            for (int i = 0; i <= 16; ++i)
            {
                for (int j = 0; j + i <= 16; ++j)
                {
                    const T u = static_cast<T>(i) / 16;
                    const T v = static_cast<T>(j) / 16;
                    const Vec3<T> s = t2.a * (static_cast<T>(1) - u - v) + t2.b * u + t2.c * v;
                    REQUIRE(crd::math::distance_squared(p, s) >= best - static_cast<T>(1e-3));
                }
            }
            REQUIRE(crd::math::distance_squared(q, p) == Catch::Approx(best).margin(tol<T>()));
        }
    }
}

TEMPLATE_TEST_CASE("closest_point 3D -- boxes & rounds", "[geometry][closest_point]", float, double)
{
    using T = TestType;
    SECTION("AABB3 -- clamp, distance companions")
    {
        const AABB3<T> box(Vec3<T>(-1, -1, -1), Vec3<T>(1, 1, 1));
        close3(closest_point(box, Vec3<T>(3, static_cast<T>(0.5), -7)), Vec3<T>(1, static_cast<T>(0.5), -1));
        REQUIRE(distance(box, Vec3<T>(4, 0, 0)) == Catch::Approx(static_cast<T>(3)).margin(tol<T>()));
        close3(closest_point(box, Vec3<T>(0, 0, 0)), Vec3<T>(0, 0, 0)); // inside -> returns itself
    }
    SECTION("OBB3 -- axis-aligned reduces to AABB; 45deg-rotated about z")
    {
        const OBB3<T> aa(Vec3<T>(0, 0, 0), Vec3<T>(1, 1, 1), Mat3<T>::identity());
        close3(closest_point(aa, Vec3<T>(5, static_cast<T>(0.5), -2)), Vec3<T>(1, static_cast<T>(0.5), -1));

        const T c = static_cast<T>(0.70710678118654752440);                       // cos 45deg
        const Mat3<T> rot(Vec3<T>(c, c, 0), Vec3<T>(-c, c, 0), Vec3<T>(0, 0, 1)); // columns = rotated axes
        const OBB3<T> obb(Vec3<T>(0, 0, 0), Vec3<T>(2, 1, 1), rot);
        // Query (10,0,0): local-frame projection (10c, -10c, 0) clamps to (2, -1, 0),
        // world = c0*2 + c1*(-1) = (2c,2c,0) + (c,-c,0) = (3c, c, 0).
        const Vec3<T> q = closest_point(obb, Vec3<T>(10, 0, 0));
        close3(q, Vec3<T>(static_cast<T>(3) * c, c, 0));
    }
    SECTION("Sphere -- surface point; inside still maps to the surface")
    {
        const Sphere<T> sph(Vec3<T>(1, 2, 3), static_cast<T>(2));
        close3(closest_point(sph, Vec3<T>(1, 2, 3) + Vec3<T>(0, 0, 9)), Vec3<T>(1, 2, 3) + Vec3<T>(0, 0, 2));
        close3(closest_point(sph, Vec3<T>(1, 2, 3) + Vec3<T>(static_cast<T>(0.5), 0, 0)),
               Vec3<T>(1, 2, 3) + Vec3<T>(2, 0, 0));                           // inside
        close3(closest_point(sph, sph.center), sph.center + Vec3<T>(2, 0, 0)); // exactly at center -> +x tiebreak
        REQUIRE(distance(sph, Vec3<T>(1, 2, 3) + Vec3<T>(0, 5, 0)) ==
                Catch::Approx(static_cast<T>(3)).margin(tol<T>()));
    }
    SECTION("Capsule3 -- cylinder body and the spherical caps")
    {
        const Capsule3<T> cap(Vec3<T>(0, 0, 0), Vec3<T>(0, 0, 10), static_cast<T>(1));
        close3(closest_point(cap, Vec3<T>(5, 0, 4)), Vec3<T>(1, 0, 4));   // body
        close3(closest_point(cap, Vec3<T>(0, 3, 0)), Vec3<T>(0, 1, 0));   // bottom cap
        close3(closest_point(cap, Vec3<T>(0, 0, 13)), Vec3<T>(0, 0, 11)); // top cap, along the axis
        close3(closest_point(cap, Vec3<T>(0, 0, 5)), Vec3<T>(1, 0, 5));   // on the spine -> +x tiebreak
    }
}

TEMPLATE_TEST_CASE("closest_points 3D -- Segment3 <-> Segment3", "[geometry][closest_point]", float, double)
{
    using T = TestType;
    SECTION("skew lines")
    {
        const Segment3<T> s1(Vec3<T>(0, 0, 0), Vec3<T>(1, 0, 0));  // along +x at z=0
        const Segment3<T> s2(Vec3<T>(0, 1, 5), Vec3<T>(0, 1, -5)); // along z at x=0,y=1
        Vec3<T> c1{};
        Vec3<T> c2{};
        closest_points(s1, s2, c1, c2);
        close3(c1, Vec3<T>(0, 0, 0));
        close3(c2, Vec3<T>(0, 1, 0));
        REQUIRE(distance(s1, s2) == Catch::Approx(static_cast<T>(1)).margin(tol<T>()));
    }
    SECTION("parallel -- deterministic pick")
    {
        const Segment3<T> s1(Vec3<T>(0, 0, 0), Vec3<T>(4, 0, 0));
        const Segment3<T> s2(Vec3<T>(1, 2, 0), Vec3<T>(7, 2, 0));
        Vec3<T> c1{};
        Vec3<T> c2{};
        closest_points(s1, s2, c1, c2);
        REQUIRE(distance(s1, s2) == Catch::Approx(static_cast<T>(2)).margin(tol<T>()));
        REQUIRE(approx_equal_abs(c1.y, static_cast<T>(0), tol<T>()));
        REQUIRE(approx_equal_abs(c2.y, static_cast<T>(2), tol<T>()));
        REQUIRE(approx_equal_abs(c1.x, c2.x, tol<T>())); // perpendicular link
    }
    SECTION("touching")
    {
        const Segment3<T> s1(Vec3<T>(0, 0, 0), Vec3<T>(2, 0, 0));
        const Segment3<T> s2(Vec3<T>(2, 0, 0), Vec3<T>(2, 5, 0));
        Vec3<T> c1{};
        Vec3<T> c2{};
        closest_points(s1, s2, c1, c2);
        close3(c1, Vec3<T>(2, 0, 0));
        close3(c2, Vec3<T>(2, 0, 0));
        REQUIRE(distance(s1, s2) == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));
    }
    SECTION("degenerate (one is a point)")
    {
        const Segment3<T> s1(Vec3<T>(3, 3, 3), Vec3<T>(3, 3, 3)); // a point
        const Segment3<T> s2(Vec3<T>(0, 0, 0), Vec3<T>(10, 0, 0));
        Vec3<T> c1{};
        Vec3<T> c2{};
        closest_points(s1, s2, c1, c2);
        close3(c1, Vec3<T>(3, 3, 3));
        close3(c2, Vec3<T>(3, 0, 0));
    }
}

// ===========================================================================
// 2D
// ===========================================================================

TEMPLATE_TEST_CASE("closest_point 2D -- linear & boundary primitives", "[geometry][closest_point]", float, double)
{
    using T = TestType;
    SECTION("Line2 / Segment2 / Ray2")
    {
        const Line2<T> line(Vec2<T>(0, 1), Vec2<T>(2, 0)); // y = 1
        close2(closest_point(line, Vec2<T>(5, 9)), Vec2<T>(5, 1));
        REQUIRE(distance(line, Vec2<T>(3, 4)) == Catch::Approx(static_cast<T>(3)).margin(tol<T>()));
        REQUIRE(signed_distance(line, Vec2<T>(3, 4)) ==
                Catch::Approx(static_cast<T>(3)).margin(tol<T>())); // perp = (0,1)
        REQUIRE(signed_distance(line, Vec2<T>(3, -2)) == Catch::Approx(static_cast<T>(-3)).margin(tol<T>()));

        const Segment2<T> seg(Vec2<T>(0, 0), Vec2<T>(10, 0));
        close2(closest_point(seg, Vec2<T>(3, 4)), Vec2<T>(3, 0));
        close2(closest_point(seg, Vec2<T>(-7, 1)), Vec2<T>(0, 0));
        close2(closest_point(seg, Vec2<T>(50, 1)), Vec2<T>(10, 0));

        const Ray2<T> ray(Vec2<T>(0, 0), Vec2<T>(0, 1));
        close2(closest_point(ray, Vec2<T>(3, 5)), Vec2<T>(0, 5));
        close2(closest_point(ray, Vec2<T>(3, -5)), Vec2<T>(0, 0));
    }
    SECTION("AABB2 / OBB2")
    {
        const AABB2<T> box(Vec2<T>(-1, -1), Vec2<T>(1, 1));
        close2(closest_point(box, Vec2<T>(4, static_cast<T>(0.25))), Vec2<T>(1, static_cast<T>(0.25)));
        close2(closest_point(box, Vec2<T>(0, 0)), Vec2<T>(0, 0));
        REQUIRE(distance(box, Vec2<T>(0, 5)) == Catch::Approx(static_cast<T>(4)).margin(tol<T>()));

        const T c = static_cast<T>(0.70710678118654752440);
        const Mat2<T> rot(Vec2<T>(c, c), Vec2<T>(-c, c)); // 45deg
        const OBB2<T> obb(Vec2<T>(0, 0), Vec2<T>(2, 1), rot);
        // local (10c, -10c) clamps to (2, -1) -> world c0*2 + c1*(-1) = (3c, c)
        close2(closest_point(obb, Vec2<T>(10, 0)), Vec2<T>(static_cast<T>(3) * c, c));
    }
    SECTION("Circle / Capsule2")
    {
        const Circle<T> circ(Vec2<T>(1, 1), static_cast<T>(2));
        close2(closest_point(circ, Vec2<T>(1, 9)), Vec2<T>(1, 3));
        close2(closest_point(circ, Vec2<T>(static_cast<T>(1.5), 1)), Vec2<T>(3, 1)); // inside -> boundary
        close2(closest_point(circ, circ.center), circ.center + Vec2<T>(2, 0));       // at center -> +x tiebreak

        const Capsule2<T> cap(Vec2<T>(0, 0), Vec2<T>(0, 10), static_cast<T>(1));
        close2(closest_point(cap, Vec2<T>(5, 4)), Vec2<T>(1, 4));
        close2(closest_point(cap, Vec2<T>(0, -3)), Vec2<T>(0, -1)); // bottom cap
        close2(closest_point(cap, Vec2<T>(0, 5)), Vec2<T>(1, 5));   // on the spine -> +x tiebreak
    }
}

TEMPLATE_TEST_CASE("closest_point 2D -- Triangle2 (Ericson Voronoi regions)", "[geometry][closest_point]", float, double)
{
    using T = TestType;
    const Triangle2<T> tri(Vec2<T>(0, 0), Vec2<T>(1, 0), Vec2<T>(0, 1));

    SECTION("vertex / edge / interior")
    {
        close2(closest_point(tri, Vec2<T>(-2, -3)), Vec2<T>(0, 0));                                    // A
        close2(closest_point(tri, Vec2<T>(4, -2)), Vec2<T>(1, 0));                                     // B
        close2(closest_point(tri, Vec2<T>(-3, 5)), Vec2<T>(0, 1));                                     // C
        close2(closest_point(tri, Vec2<T>(static_cast<T>(0.5), -4)), Vec2<T>(static_cast<T>(0.5), 0)); // AB
        close2(closest_point(tri, Vec2<T>(-4, static_cast<T>(0.5))), Vec2<T>(0, static_cast<T>(0.5))); // AC
        close2(closest_point(tri, Vec2<T>(2, 2)), Vec2<T>(static_cast<T>(0.5), static_cast<T>(0.5)));  // BC
        close2(closest_point(tri, Vec2<T>(static_cast<T>(0.2), static_cast<T>(0.2))),
               Vec2<T>(static_cast<T>(0.2), static_cast<T>(0.2))); // interior -> identity
    }
    SECTION("degenerate (collinear)")
    {
        const Triangle2<T> flat(Vec2<T>(0, 0), Vec2<T>(2, 0), Vec2<T>(5, 0));
        close2(closest_point(flat, Vec2<T>(1, 3)), Vec2<T>(1, 0));
        close2(closest_point(flat, Vec2<T>(-2, 1)), Vec2<T>(0, 0));
        close2(closest_point(flat, Vec2<T>(99, 1)), Vec2<T>(5, 0));
    }
    SECTION("property: minimises over a barycentric sample")
    {
        const Triangle2<T> t2(Vec2<T>(-1, 2), Vec2<T>(3, -1), Vec2<T>(0, 4));
        Rng rng(0x2D12U);
        for (int it = 0; it < 64; ++it)
        {
            const Vec2<T> p = rnd2<T>(rng, static_cast<T>(-5), static_cast<T>(5));
            const T best = distance_squared(t2, p);
            for (int i = 0; i <= 16; ++i)
            {
                for (int j = 0; j + i <= 16; ++j)
                {
                    const T u = static_cast<T>(i) / 16;
                    const T v = static_cast<T>(j) / 16;
                    const Vec2<T> s = t2.a * (static_cast<T>(1) - u - v) + t2.b * u + t2.c * v;
                    REQUIRE(crd::math::distance_squared(p, s) >= best - static_cast<T>(1e-3));
                }
            }
        }
    }
}

TEMPLATE_TEST_CASE("closest_points 2D -- Segment2 <-> Segment2", "[geometry][closest_point]", float, double)
{
    using T = TestType;
    SECTION("crossing -> distance 0")
    {
        const Segment2<T> s1(Vec2<T>(-1, 0), Vec2<T>(1, 0));
        const Segment2<T> s2(Vec2<T>(0, -1), Vec2<T>(0, 1));
        Vec2<T> c1{};
        Vec2<T> c2{};
        closest_points(s1, s2, c1, c2);
        REQUIRE(distance(s1, s2) == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));
        close2(c1, Vec2<T>(0, 0), static_cast<T>(2e-3));
        close2(c2, Vec2<T>(0, 0), static_cast<T>(2e-3));
    }
    SECTION("parallel")
    {
        const Segment2<T> s1(Vec2<T>(0, 0), Vec2<T>(4, 0));
        const Segment2<T> s2(Vec2<T>(10, 3), Vec2<T>(14, 3));
        Vec2<T> c1{};
        Vec2<T> c2{};
        closest_points(s1, s2, c1, c2);
        close2(c1, Vec2<T>(4, 0));
        close2(c2, Vec2<T>(10, 3));
        REQUIRE(distance(s1, s2) == Catch::Approx(static_cast<T>(std::sqrt(static_cast<T>(45)))).margin(tol<T>()));
    }
}

// ---- Cylinder3 ------------------------------------------------------------
// Phase 3.1.7 v1-close debt payment.

TEMPLATE_TEST_CASE("closest_point 3D -- Cylinder3 (axis-aligned)", "[geometry][closest_point]", float, double)
{
    using T = TestType;
    // Unit cylinder along Y, radius 1, from y=0 to y=2.
    const Cylinder3<T> cyl(Vec3<T>(0, 0, 0), Vec3<T>(0, 2, 0), static_cast<T>(1));

    SECTION("interior point -> identity")
    {
        const Vec3<T> p(static_cast<T>(0.3), static_cast<T>(1.0), static_cast<T>(-0.2));
        close3(closest_point(cyl, p), p);
        REQUIRE(distance(cyl, p) == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));
    }
    SECTION("outside body (above axis) -> radial clamp at the same Y")
    {
        const Vec3<T> p(static_cast<T>(3), static_cast<T>(1), 0);
        close3(closest_point(cyl, p), Vec3<T>(static_cast<T>(1), static_cast<T>(1), 0));
        REQUIRE(distance(cyl, p) == Catch::Approx(static_cast<T>(2)).margin(tol<T>()));
    }
    SECTION("beyond top cap, off-axis -> radial clamp on top disc")
    {
        const Vec3<T> p(static_cast<T>(3), static_cast<T>(5), 0);
        // Cap at y=2, radial clamp to radius 1 in the disc plane.
        close3(closest_point(cyl, p), Vec3<T>(static_cast<T>(1), static_cast<T>(2), 0));
    }
    SECTION("beyond top cap, on axis -> top center")
    {
        const Vec3<T> p(0, static_cast<T>(5), 0);
        close3(closest_point(cyl, p), Vec3<T>(0, static_cast<T>(2), 0));
        REQUIRE(distance(cyl, p) == Catch::Approx(static_cast<T>(3)).margin(tol<T>()));
    }
    SECTION("degenerate axis (a == b) -> collapses to a")
    {
        const Cylinder3<T> deg(Vec3<T>(1, 2, 3), Vec3<T>(1, 2, 3), static_cast<T>(1));
        const Vec3<T> p(10, 20, 30);
        close3(closest_point(deg, p), Vec3<T>(1, 2, 3));
    }
}

// ---- Tetrahedron ----------------------------------------------------------

TEMPLATE_TEST_CASE("closest_point 3D -- Tetrahedron", "[geometry][closest_point]", float, double)
{
    using T = TestType;
    // Canonical unit tetrahedron with vertices at origin + 3 unit axes.
    const Tetrahedron<T> tet(Vec3<T>(0, 0, 0), Vec3<T>(1, 0, 0), Vec3<T>(0, 1, 0), Vec3<T>(0, 0, 1));

    SECTION("interior point -> identity")
    {
        const Vec3<T> p(static_cast<T>(0.2), static_cast<T>(0.2), static_cast<T>(0.2));
        close3(closest_point(tet, p), p);
        REQUIRE(distance(tet, p) == Catch::Approx(static_cast<T>(0)).margin(tol<T>()));
    }
    SECTION("outside along -X axis -> closest on the y/z face through origin")
    {
        const Vec3<T> p(static_cast<T>(-1), static_cast<T>(0.25), static_cast<T>(0.25));
        // Closest point is the same y/z on the x=0 face (origin/y-axis/z-axis triangle).
        close3(closest_point(tet, p), Vec3<T>(0, static_cast<T>(0.25), static_cast<T>(0.25)));
    }
    SECTION("vertex projection -> matches vertex")
    {
        // Far above the (1,0,0) vertex along +X.
        const Vec3<T> p(static_cast<T>(5), 0, 0);
        close3(closest_point(tet, p), Vec3<T>(static_cast<T>(1), 0, 0));
        REQUIRE(distance(tet, p) == Catch::Approx(static_cast<T>(4)).margin(tol<T>()));
    }
    SECTION("centroid -> closest is centroid (inside)")
    {
        const Vec3<T> c(static_cast<T>(0.25), static_cast<T>(0.25), static_cast<T>(0.25));
        close3(closest_point(tet, c), c);
    }
    SECTION("matches brute-force per-face min over a random sample")
    {
        // For 12 random outside points, the result must equal the min over
        // closest_point(face_i, p) for i in {0..3}.
        for (int trial = 0; trial < 12; ++trial)
        {
            const T sx = static_cast<T>(((trial * 31) % 7) - 3);
            const T sy = static_cast<T>(((trial * 17) % 5) - 2);
            const T sz = static_cast<T>(((trial * 13) % 9) - 4);
            const Vec3<T> p(sx, sy, sz);
            const Vec3<T> got = closest_point(tet, p);
            const Triangle3<T> faces[4] = {
                Triangle3<T>(tet.a, tet.b, tet.c), Triangle3<T>(tet.a, tet.c, tet.d),
                Triangle3<T>(tet.a, tet.d, tet.b), Triangle3<T>(tet.b, tet.d, tet.c),
            };
            Vec3<T> best = closest_point(faces[0], p);
            T best_d2 = crd::math::distance_squared(best, p);
            for (int i = 1; i < 4; ++i)
            {
                const Vec3<T> cp = closest_point(faces[i], p);
                const T d2 = crd::math::distance_squared(cp, p);
                if (d2 < best_d2) { best_d2 = d2; best = cp; }
            }
            // If the query is inside, both agree on `p`. If outside, both
            // agree on the same face point (modulo ULP).
            if (contains(tet, p))
            {
                close3(got, p, static_cast<T>(1e-3));
            }
            else
            {
                REQUIRE(distance_squared(tet, p) ==
                        Catch::Approx(static_cast<T>(best_d2)).margin(static_cast<T>(1e-4)));
            }
        }
    }
}
